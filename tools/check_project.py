#!/usr/bin/env python3
import argparse
import re
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def read_text(path):
    return (ROOT / path).read_text(encoding="utf-8")


def read_text_if_exists(path):
    full = ROOT / path
    if not full.exists():
        return ""
    return full.read_text(encoding="utf-8")


def fail(message):
    print(f"[fail] {message}", file=sys.stderr)
    raise SystemExit(1)


def ok(message):
    print(f"[ok] {message}")


def parse_make_int(text, name):
    m = re.search(rf"^{re.escape(name)}\s*:?=\s*(\d+)\s*$", text, re.M)
    if not m:
        fail(f"missing {name} in Makefile")
    return int(m.group(1))


def parse_make_words(text, name):
    m = re.search(rf"^{re.escape(name)}\s*:?=\s*(.*?)\s*$", text, re.M)
    if not m:
        fail(f"missing {name} in Makefile")
    return m.group(1).split()


def parse_define_number(text, name, seen=None):
    seen = set() if seen is None else seen
    if name in seen:
        fail(f"recursive #define {name}")
    seen.add(name)
    m = re.search(rf"^\s*#define\s+{re.escape(name)}\s+([^/\r\n]+)", text, re.M)
    if not m:
        fail(f"missing #define {name}")
    value = m.group(1).strip()
    wrapped = re.fullmatch(r"UINT(?:8|16|32|64)_C\((0x[0-9A-Fa-f]+|\d+)\)", value)
    if wrapped:
        return int(wrapped.group(1), 0)
    literal = re.fullmatch(r"(0x[0-9A-Fa-f]+|\d+)[uUlL]*", value)
    if literal:
        return int(literal.group(1), 0)
    alias = re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", value)
    if alias:
        return parse_define_number(text, value, seen)
    fail(f"unsupported value for #define {name}: {value}")


def parse_c_int(text, name):
    m = re.search(rf"\b{re.escape(name)}\s*=\s*(\d+)\b", text)
    if not m:
        fail(f"missing {name}")
    return int(m.group(1))


def parse_boot_stack_top(text):
    m = re.search(r"^\s*mov\s+esp,\s*(0x[0-9A-Fa-f]+|\d+)\b", text, re.M)
    if not m:
        fail("missing protected-mode boot stack setup")
    return int(m.group(1), 0)


def elf_section_range(path, wanted):
    data = path.read_bytes()
    if len(data) < 64 or data[:4] != b"\x7fELF" or data[4] != 2 or data[5] != 1:
        fail(f"{path.name}: unsupported ELF64 file")
    (_ident, _etype, _emachine, _version, _entry, _phoff, shoff, _flags,
     _ehsize, _phentsize, _phnum, shentsize, shnum, shstrndx) = struct.unpack_from(
        "<16sHHIQQQIHHHHHH", data, 0
    )
    if shentsize != 64 or shoff + shnum * shentsize > len(data) or shstrndx >= shnum:
        fail(f"{path.name}: invalid section headers")
    shstr_off = shoff + shstrndx * shentsize
    _name, _type, _flags, _addr, shstrtab_off, shstrtab_size, *_rest = struct.unpack_from("<IIQQQQIIQQ", data, shstr_off)
    if shstrtab_off + shstrtab_size > len(data):
        fail(f"{path.name}: invalid section-name table")
    names = data[shstrtab_off:shstrtab_off + shstrtab_size]

    def section_name(offset):
        if offset >= len(names):
            return ""
        end = names.find(b"\x00", offset)
        if end < 0:
            end = len(names)
        return names[offset:end].decode("ascii", errors="ignore")

    for i in range(shnum):
        off = shoff + i * shentsize
        name_off, _type, _flags, addr, _offset, size, *_rest = struct.unpack_from("<IIQQQQIIQQ", data, off)
        if section_name(name_off) == wanted:
            return addr, addr + size
    fail(f"{path.name}: missing section {wanted}")


def check_image_layout():
    makefile = read_text("Makefile")
    minifs_h = read_text("src/kernel/fs/minifs/minifs.h")
    boot_start = parse_make_int(makefile, "BOOT_PARTITION_START")
    boot_sectors = parse_make_int(makefile, "BOOT_PARTITION_SECTORS")
    fs_start = parse_make_int(makefile, "FS_START_SECTOR")
    fs_sectors = parse_make_int(makefile, "FS_SECTORS")
    minifs_start = parse_c_int(minifs_h, "MINIFS_LBA_START")
    minifs_sectors = parse_c_int(minifs_h, "MINIFS_SECTORS")

    if fs_start != minifs_start:
        fail(f"FS_START_SECTOR mismatch: Makefile={fs_start}, minifs.h={minifs_start}")
    if fs_sectors != minifs_sectors:
        fail(f"FS_SECTORS mismatch: Makefile={fs_sectors}, minifs.h={minifs_sectors}")
    boot_end = boot_start + boot_sectors
    if boot_end > fs_start:
        fail(f"boot partition ends at LBA {boot_end}, overlaps FS start LBA {fs_start}")

    kernel_elf = ROOT / "build/obj/kernel/kernel.elf"
    if not kernel_elf.exists():
        fail("missing build/obj/kernel/kernel.elf; run make first")
    kernel_size = kernel_elf.stat().st_size
    max_kernel = boot_sectors * 512
    if kernel_size > max_kernel:
        fail(f"kernel.elf is {kernel_size} bytes, boot partition capacity is {max_kernel}")

    image = ROOT / "build/buzzos.img"
    if image.exists():
        expected = (fs_start + fs_sectors) * 512
        actual = image.stat().st_size
        if actual != expected:
            fail(f"buzzos.img size is {actual}, expected {expected}")

    ok(f"image layout: kernel ELF {kernel_size}/{max_kernel} bytes, boot LBA {boot_start}..{boot_end - 1}, fs LBA {fs_start}..{fs_start + fs_sectors - 1}")


def check_kernel_memory_layout():
    boot = read_text("src/kernel/arch/x86_64/mb2_entry.asm")
    pmm_c = read_text("src/kernel/mm/pmm.c")
    kernel_elf = ROOT / "build/obj/kernel/kernel.elf"
    if not kernel_elf.exists():
        fail("missing build/obj/kernel/kernel.elf; run make first")
    text_start, _text_end = elf_section_range(kernel_elf, ".text")
    _bss_start, bss_end = elf_section_range(kernel_elf, ".bss")
    stack_start, stack_top = elf_section_range(kernel_elf, ".boot_stack")
    managed_limit = parse_define_number(pmm_c, "PMM_MANAGED_LIMIT")
    stack_reserve = 0x10000
    vga_hole_start = 0xA0000
    vga_hole_end = 0x100000
    if text_start < vga_hole_end and bss_end > vga_hole_start:
        fail(f"kernel image 0x{text_start:X}..0x{bss_end:X} overlaps VGA/BIOS hole 0x{vga_hole_start:X}..0x{vga_hole_end:X}")
    if stack_top > managed_limit:
        fail(f"boot stack top 0x{stack_top:X} exceeds PMM managed limit 0x{managed_limit:X}")
    if stack_top - stack_start < stack_reserve:
        fail(f"boot stack is only {stack_top - stack_start} bytes")
    if bss_end > stack_start:
        fail(f"kernel .bss ends at 0x{bss_end:X}, overlapping boot stack at 0x{stack_start:X}")
    for snippet in ["__kernel_end", "kernel_end - kernel_start"]:
        if snippet not in pmm_c:
            fail(f"PMM does not reserve linker-placed boot stack: missing {snippet}")
    ok(f"kernel memory: image 0x{text_start:06X}..0x{bss_end:06X}, boot stack 0x{stack_start:06X}..0x{stack_top:06X}")


def check_user_bounds():
    bounds_h = read_text("src/kernel/arch/x86_64/user_bounds.h")
    elf_c = read_text("src/kernel/core/elf.c")
    syscall_h = read_text("src/kernel/syscall/syscall_internal.h")
    paging_c = read_text("src/kernel/arch/x86_64/paging.c")
    user_h = read_text("src/kernel/arch/x86_64/user.h")

    for path, text in [
        ("src/kernel/core/elf.c", elf_c),
        ("src/kernel/syscall/syscall_internal.h", syscall_h),
        ("src/kernel/arch/x86_64/paging.c", paging_c),
        ("src/kernel/arch/x86_64/user.h", user_h),
    ]:
        if '#include "user_bounds.h"' not in text:
            fail(f"{path} should include shared user_bounds.h")

    load_start = parse_define_number(bounds_h, "USER_LOAD_START")
    load_end = parse_define_number(bounds_h, "USER_LOAD_END")
    ptr_start = parse_define_number(bounds_h, "USER_PTR_START")
    ptr_end = parse_define_number(bounds_h, "USER_PTR_END")
    space_start = parse_define_number(bounds_h, "USER_SPACE_START")
    space_end = parse_define_number(bounds_h, "USER_SPACE_END")
    stack_top = parse_define_number(bounds_h, "USER_DEFAULT_STACK_TOP")

    if not (load_start == ptr_start == space_start):
        fail("user start constants differ across ELF/syscall/paging")
    if not (load_start < load_end <= stack_top < ptr_end == space_end):
        fail("user load/stack/pointer bounds are inconsistent")
    if stack_top - load_end < 0x10000:
        fail("user stack is too close to the ELF load window")

    ok(f"user bounds: load 0x{load_start:06X}..0x{load_end:06X}, stack 0x{stack_top:06X}, mapped to 0x{ptr_end:06X}")
    return load_start, load_end, stack_top


def check_user_fault_isolation():
    bounds_h = read_text("src/kernel/arch/x86_64/user_bounds.h")
    paging_c = read_text("src/kernel/arch/x86_64/paging.c")
    pmm_c = read_text("src/kernel/mm/pmm.c")
    user_c = read_text("src/kernel/arch/x86_64/user.c")
    exec_c = read_text("src/kernel/core/exec.c")
    idt_c = read_text("src/kernel/arch/x86_64/idt.c")
    syscall_c = read_text("src/kernel/syscall/syscall.c")
    makefile = read_text("Makefile")
    kernel_c = read_text("src/kernel/core/kernel.c")
    shell_c = read_text("src/user/bin/shell.c")
    smoke_ps1 = read_text("scripts/smoke.ps1")

    for snippet in [
        "paging_user_range_accessible",
        "!(*pte & PAGE_USER)",
        "write && !(*pte & PAGE_RW)",
        "paging_set_user_range_writable",
    ]:
        if snippet not in paging_c:
            fail(f"page-aware user access checks are missing: {snippet}")

    for snippet in [
        "paging_user_range_accessible(ptr, len, 0)",
        "paging_user_range_accessible(ptr, len, 1)",
    ]:
        if snippet not in syscall_c:
            fail(f"syscall pointer validation is missing: {snippet}")

    for snippet in [
        "USER_TRAMPOLINE_BASE",
        "jmp rdx",
    ]:
        if snippet not in bounds_h + "\n" + user_c:
            fail(f"private user trampoline support is missing: {snippet}")
    for snippet in [
        "paging_map_user_range_in_space(",
        "user_install_trampoline_in_space(cr3)",
        "paging_set_user_range_writable_in_space(",
    ]:
        if snippet not in exec_c:
            fail(f"exec trampoline setup is missing: {snippet}")
    if "0x1FF000" in paging_c + "\n" + pmm_c + "\n" + user_c:
        fail("the legacy shared writable trampoline at 0x1FF000 is still present")

    for snippet in [
        "(frame->cs & 3u) == 3u",
        "task_exit_process_code(vector ? -(int)vector : -1)",
        "Terminating faulting user task",
    ]:
        if snippet not in idt_c:
            fail(f"user exception isolation is missing: {snippet}")

    fixture_text = makefile + "\n" + kernel_c + "\n" + shell_c + "\n" + smoke_ps1
    for snippet in [
        "FAULTTEST_ELF",
        "/bin/faulttest",
        "cmd_badptrtest",
        '"badptrtest"',
        '"exec /bin/faulttest"',
        "Terminating faulting user task",
    ]:
        if snippet not in fixture_text:
            fail(f"user-fault regression coverage is missing: {snippet}")

    ok("user isolation: page-aware pointers, private read-only trampoline, and per-process fault termination are covered")


def check_runtime_lifecycle():
    idt_c = read_text("src/kernel/arch/x86_64/idt.c")
    irq_h = read_text("src/kernel/arch/x86_64/irq.h")
    task_c = read_text("src/kernel/sched/task.c")
    sys_proc = read_text("src/kernel/syscall/sys_proc.c")
    sys_net = read_text("src/kernel/syscall/sys_net.c")
    gui_c = read_text("src/user/bin/gui.c")
    terminal_c = read_text("src/user/bin/terminal.c")
    gui_smoke = read_text("scripts/gui-smoke.ps1")
    smoke = read_text("scripts/smoke.ps1")

    for snippet in ["irq_save", "irq_restore", "pushfq; popq %0; cli"]:
        if snippet not in irq_h:
            fail(f"shared IRQ-state helper is missing: {snippet}")
    for snippet in [
        "IDT_GATE_TRAP_USER",
        "{SYSCALL_VECTOR, syscall_stub, IDT_GATE_TRAP_USER}",
    ]:
        if snippet not in idt_c:
            fail(f"preemptible syscall gate is missing: {snippet}")
    schedule_match = re.search(r"static void schedule\(void\) \{(.*?)\n\}", task_c, re.S)
    if not schedule_match or "irq_save()" not in schedule_match.group(1) or "irq_restore(irq_flags)" not in schedule_match.group(1):
        fail("scheduler does not preserve the caller's interrupt state")
    if '__asm__ volatile("sti")' in schedule_match.group(1):
        fail("scheduler still enables interrupts unconditionally")

    for snippet in [
        "task_exit_process_code",
        "syscall_cleanup_process(id)",
        "syscall_release_thread(id)",
    ]:
        if snippet not in task_c:
            fail(f"process/thread lifecycle cleanup is missing: {snippet}")
    for snippet in [
        "process_thread_slots",
        "USER_THREAD_STACK_SLOTS",
        "USER_MAIN_STACK_SIZE",
        "syscall_release_thread",
        "sys_net_cleanup_owner(task_id)",
    ]:
        if snippet not in sys_proc:
            fail(f"thread stack/socket ownership cleanup is missing: {snippet}")
    for snippet in ["sys_net_cleanup_owner", "socket_clear", "s->owner != owner"]:
        if snippet not in sys_net:
            fail(f"socket owner cleanup is missing: {snippet}")

    # The desktop once hosted a single embedded terminal, reaped by
    # join(term_reader_tid) / waitpid(term_pid).  Terminal is now an ordinary
    # GUI app and every app owns a reader thread, so the invariant these
    # assertions protect -- that a closing window joins its reader and reaps
    # its process rather than leaking either -- lives in close_window().
    for snippet in [
        "shutdown_desktop",
        "join(app_sessions[slot].reader_tid)",
        "waitpid(app_sessions[slot].pid",
        "desktop_dirty",
        "tick - last_render_tick >= 60u",
        "sync_app_size(finished_resize",
    ]:
        if snippet not in gui_c:
            fail(f"desktop lifecycle/event-driven rendering is missing: {snippet}")
    for snippet in ["threadreusetest", "socketleak: opened 8", "3072"]:
        if snippet not in smoke:
            fail(f"resource/TCP smoke coverage is missing: {snippet}")
    for snippet in ["textedit-maximized", "filemanager", "filemanager-textedit",
                    "filemanager-terminal-exec", "app protocol ended",
                    "many-windows", "dock-expanded",
                    "Move-MouseRelative", "Click-Left"]:
        if snippet not in gui_smoke:
            fail(f"live TextEdit resize coverage is missing: {snippet}")

    ok("runtime lifecycle: IRQ state, process exit, thread/socket reuse, GUI shutdown/live resize, and idle redraw are covered")


def check_elf_loader_hardening():
    elf_c = read_text("src/kernel/core/elf.c")
    elf_h = read_text("src/kernel/core/elf.h")
    exec_c = read_text("src/kernel/core/exec.c")
    shell_c = read_text("src/user/bin/shell.c")
    smoke_ps1 = read_text("scripts/smoke.ps1")

    for snippet in [
        "uintptr_t elf_load_into_space(uintptr_t cr3",
        "static int range_in_file",
        "static int range_in_load_window",
        "static int entry_in_segment",
        "eh->e_ehsize != sizeof(*eh)",
        "eh->e_phentsize != sizeof(struct elf64_phdr)",
        "range_in_file(eh->e_phoff, bytes, size)",
        "ph->p_filesz <= ph->p_memsz",
        "range_in_file(ph->p_offset, ph->p_filesz, size)",
        "range_in_load_window(ph->p_vaddr, ph->p_memsz)",
        "entry_ok",
        "paging_copy_to_user_space",
    ]:
        if snippet not in elf_c:
            fail(f"ELF loader hardening is missing: {snippet}")

    for snippet in [
        "uintptr_t elf_load_into_space(uintptr_t cr3",
        "uintptr_t elf_load_file_into_space(uintptr_t cr3",
    ]:
        if snippet not in elf_h:
            fail(f"elf.h does not expose the address-space ELF loader: {snippet}")

    for snippet in [
        "elf_load_into_space(",
        "elf_load_file_into_space(",
        "paging_destroy_user_space(proc_cr3)",
        'serial_puts("[exec] bad ELF\\n")',
    ]:
        if snippet not in exec_c:
            fail(f"exec bad-ELF path is missing: {snippet}")

    for snippet in [
        "cmd_elfbadtest",
        "make_bad_elf",
        "run_bad_elf_case",
        'run_bad_elf_case("vaddr"',
        'run_bad_elf_case("filesz"',
        'run_bad_elf_case("memsz"',
        'run_bad_elf_case("entry"',
        "spawn_process_args(path, argv, 1, SPAWN_FLAG_SILENT)",
    ]:
        if snippet not in shell_c:
            fail(f"shell is missing bad ELF runtime coverage: {snippet}")

    for snippet in [
        "elfbadtest",
        "elfbad: vaddr -1",
        "elfbad: filesz -1",
        "elfbad: memsz -1",
        "elfbad: entry -1",
        "elfbad: ok",
    ]:
        if snippet not in smoke_ps1:
            fail(f"smoke.ps1 is missing bad ELF coverage: {snippet}")

    ok("ELF loader: size-aware validation rejects malformed runtime exec fixtures")


def check_dynamic_heap():
    syscall_h = read_text("src/kernel/syscall/syscall.h")
    syscall_c = read_text("src/kernel/syscall/syscall.c")
    sys_proc = read_text("src/kernel/syscall/sys_proc.c")
    exec_c = read_text("src/kernel/core/exec.c")
    libc_h = read_text("src/user/libc/libc.h")
    libc_c = read_text("src/user/libc/libc.c")
    makefile = read_text("Makefile")
    heaptest = read_text("src/user/bin/heaptest.c")
    smoke = read_text("scripts/smoke.ps1")

    source = syscall_h + "\n" + syscall_c + "\n" + sys_proc + "\n" + exec_c
    for snippet in [
        "SYS_SBRK=54",
        "syscall_table[SYS_SBRK] = sys_sbrk",
        "process_heap_break",
        "paging_map_user_range(old_break",
        "syscall_set_heap_start(id",
    ]:
        if snippet not in source:
            fail(f"dynamic heap syscall support is missing: {snippet}")

    allocator = libc_h + "\n" + libc_c
    for snippet in [
        "void  *calloc(size_t count, size_t size)",
        "void  *realloc(void *ptr, size_t size)",
        "struct heap_block",
        "heap_merge_next",
        "syscall1(SYS_SBRK",
    ]:
        if snippet not in allocator:
            fail(f"reusable user allocator is missing: {snippet}")

    fixture = makefile + "\n" + heaptest + "\n" + smoke
    for snippet in [
        "HEAPTEST_ELF",
        "/bin/heaptest",
        "GROWN_SIZE = 320 * 1024",
        "heaptest: ok 320K realloc reuse",
    ]:
        if snippet not in fixture:
            fail(f"dynamic heap runtime coverage is missing: {snippet}")

    ok("user heap: page-backed malloc/free/calloc/realloc has >64 KiB smoke coverage")


def check_elf(path, load_start, load_end):
    data = path.read_bytes()
    if len(data) < 64 or data[:4] != b"\x7fELF":
        fail(f"{path.name}: not an ELF64 file")
    (ident, etype, emachine, version, entry, phoff, _shoff, _flags,
     ehsize, phentsize, phnum, _shentsize, _shnum, _shstrndx) = struct.unpack_from(
        "<16sHHIQQQIHHHHHH", data, 0
    )
    if ident[4] != 2 or ident[5] != 1 or etype != 2 or emachine != 62 or version != 1:
        fail(f"{path.name}: unsupported ELF header")
    if ehsize != 64 or phentsize != 56:
        fail(f"{path.name}: unexpected ELF/program-header size")
    if phoff + phnum * phentsize > len(data):
        fail(f"{path.name}: program headers outside file")

    saw_load = False
    entry_ok = False
    max_end = 0
    for i in range(phnum):
        off = phoff + i * phentsize
        ptype, flags, poff, vaddr, _paddr, filesz, memsz, _align = struct.unpack_from("<IIQQQQQQ", data, off)
        if ptype != 1:
            continue
        saw_load = True
        end = vaddr + memsz
        max_end = max(max_end, end)
        if filesz > memsz:
            fail(f"{path.name}: PT_LOAD filesz exceeds memsz")
        if poff + filesz > len(data):
            fail(f"{path.name}: PT_LOAD file range outside file")
        if vaddr < load_start or end > load_end:
            fail(f"{path.name}: PT_LOAD 0x{vaddr:X}..0x{end:X} outside load window")
        if flags & 1 and vaddr <= entry < end:
            entry_ok = True
    if not saw_load or not entry_ok:
        fail(f"{path.name}: missing valid loadable entry segment")
    return max_end


def check_user_elves(load_start, load_end, stack_top):
    user_dir = ROOT / "build/user"
    if not user_dir.exists():
        fail("missing build/user; run make first")
    elves = sorted(user_dir.glob("*.elf"))
    if not elves:
        fail("no user ELF files found")

    highest = 0
    highest_name = ""
    for elf in elves:
        end = check_elf(elf, load_start, load_end)
        if end > highest:
            highest = end
            highest_name = elf.name
    if highest >= stack_top:
        fail(f"user ELF {highest_name} reaches 0x{highest:X}, above stack 0x{stack_top:X}")
    ok(f"user ELF ranges: {len(elves)} files, highest {highest_name} ends at 0x{highest:06X}")


def parse_initrd_blobs(initrd):
    pairs = {}
    for macro, data in re.findall(
        r"#define\s+(INITRD_[A-Z0-9_]+_SIZE)\s+\d+\s+"
        r"static\s+const\s+uint8_t\s+(initrd_[A-Za-z0-9_]+_data)\[",
        initrd,
    ):
        if data in pairs:
            fail(f"duplicate initrd data symbol: {data}")
        pairs[data] = macro
    if not pairs:
        fail("no initrd blobs found in build/generated/initrd.h")
    return pairs


def check_initrd_reachability():
    initrd = read_text("build/generated/initrd.h")
    kernel_c = read_text("src/kernel/core/kernel.c")
    app_registry = read_text_if_exists("build/generated/app_registry.h")
    refs = kernel_c + "\n" + app_registry
    blobs = parse_initrd_blobs(initrd)

    missing_data = []
    missing_size = []
    for data, macro in sorted(blobs.items()):
        if not re.search(rf"\b{re.escape(data)}\b", refs):
            missing_data.append(data)
        if not re.search(rf"\b{re.escape(macro)}\b", refs):
            missing_size.append(macro)
    if missing_data:
        fail("initrd blobs generated but not reachable from kernel/app registry: " + ", ".join(missing_data))
    if missing_size:
        fail("initrd size macros generated but not reachable from kernel/app registry: " + ", ".join(missing_size))

    ramfs_paths = []
    for path, data, macro in re.findall(
        r'ramfs_register\(\s*"([^"]+)"\s*,\s*(initrd_[A-Za-z0-9_]+_data)\s*,\s*(INITRD_[A-Z0-9_]+_SIZE)\s*\)',
        kernel_c,
    ):
        if data not in blobs:
            fail(f"ramfs_register({path}) uses unknown initrd blob {data}")
        if blobs[data] != macro:
            fail(f"ramfs_register({path}) uses {macro}, expected {blobs[data]} for {data}")
        ramfs_paths.append(path)
    if not ramfs_paths:
        fail("kernel does not register any initrd blobs into ramfs")
    if "/bin/sh" not in ramfs_paths:
        fail("kernel does not register /bin/sh from initrd")

    ok(f"initrd reachability: {len(blobs)} blobs, {len(ramfs_paths)} ramfs entries")


def check_initrd_hygiene():
    makefile = read_text("Makefile")
    mkinitrd = read_text("tools/mkinitrd.py")
    user_dir = ROOT / "build/user"
    if not user_dir.exists():
        fail("missing build/user; run make first")

    if "$(OBJCOPY) --strip-sections $@" not in makefile:
        fail("Makefile does not strip user ELF section metadata before initrd embedding")
    if "BYTES_PER_LINE = 32" not in mkinitrd:
        fail("mkinitrd.py should emit compact 32-byte rows to reduce generated diff noise")

    noisy = []
    for elf in sorted(user_dir.glob("*.elf")):
        data = elf.read_bytes()
        if len(data) < 64 or data[:4] != b"\x7fELF" or data[4] != 2:
            continue
        shoff = struct.unpack_from("<Q", data, 40)[0]
        shentsize = struct.unpack_from("<H", data, 58)[0]
        shnum = struct.unpack_from("<H", data, 60)[0]
        # llvm-objcopy may leave e_shoff pointing just past the ELF64 header,
        # but zero entry size/count means no section table is reachable.
        if shentsize != 0 or shnum != 0:
            noisy.append(f"{elf.name}: shoff={shoff} shentsize={shentsize} shnum={shnum}")
        for marker in [b".comment", b"clang version", b"LLVM", b".symtab", b".strtab"]:
            if marker in data:
                noisy.append(f"{elf.name}: contains {marker.decode('ascii', errors='ignore')}")
                break
    if noisy:
        fail("user ELF initrd payloads are not stripped: " + "; ".join(noisy))

    ok("initrd hygiene: user ELF payloads are section-stripped and mkinitrd emits compact rows")


def parse_syscall_enums(text, label):
    result = {}
    for block in re.findall(r"\benum\s*(?:[A-Za-z_][A-Za-z0-9_]*)?\s*\{(.*?)\}\s*;", text, re.S):
        value = -1
        for raw in block.split(","):
            entry = raw.strip()
            if not entry:
                continue
            m = re.match(r"(SYS_[A-Z0-9_]+)(?:\s*=\s*(0x[0-9A-Fa-f]+|\d+))?$", entry)
            if not m:
                continue
            name, explicit = m.groups()
            if explicit is not None:
                value = int(explicit, 0)
            else:
                value += 1
            if name in result and result[name] != value:
                fail(f"{label}: duplicate {name} values {result[name]} and {value}")
            result[name] = value
    if not result:
        fail(f"{label}: no SYS_* enum values found")
    return result


def check_syscall_abi():
    kernel_h = read_text("src/kernel/syscall/syscall.h")
    kernel_c = read_text("src/kernel/syscall/syscall.c")
    libc_c = read_text("src/user/libc/libc.c")

    kernel = parse_syscall_enums(kernel_h, "kernel syscall.h")
    user = parse_syscall_enums(libc_c, "user libc.c")

    missing_in_user = sorted(set(kernel) - set(user))
    extra_in_user = sorted(set(user) - set(kernel))
    if missing_in_user:
        fail("user libc.c is missing syscall numbers: " + ", ".join(missing_in_user))
    if extra_in_user:
        fail("user libc.c has syscall numbers not in kernel: " + ", ".join(extra_in_user))

    mismatched = []
    for name in sorted(kernel):
        if kernel[name] != user[name]:
            mismatched.append(f"{name} kernel={kernel[name]} user={user[name]}")
    if mismatched:
        fail("syscall number mismatch: " + "; ".join(mismatched))

    table_entries = set(re.findall(r"syscall_table\[\s*(SYS_[A-Z0-9_]+)\s*\]\s*=", kernel_c))
    missing_table = sorted(set(kernel) - table_entries)
    unknown_table = sorted(table_entries - set(kernel))
    if missing_table:
        fail("kernel syscall_table missing entries: " + ", ".join(missing_table))
    if unknown_table:
        fail("kernel syscall_table references unknown syscalls: " + ", ".join(unknown_table))

    used_by_libc = set(re.findall(r"\b(SYS_[A-Z0-9_]+)\b", libc_c))
    missing_wrappers = sorted(set(kernel) - used_by_libc)
    if missing_wrappers:
        fail("user libc.c does not reference syscalls: " + ", ".join(missing_wrappers))

    ok(f"syscall ABI: {len(kernel)} numbers match user libc and kernel table")


def check_network_socket_state():
    sys_net = read_text("src/kernel/syscall/sys_net.c")
    net_c = read_text("src/kernel/net/net.c")
    net_h = read_text("src/kernel/net/net.h")
    shell_c = read_text("src/user/bin/shell.c")
    smoke_ps1 = read_text("scripts/smoke.ps1")

    if "active_tcp_socket" in sys_net:
        fail("sys_net.c still gates stream sockets through active_tcp_socket")

    m = re.search(r"struct\s+socket_entry\s*\{(.*?)\};", sys_net, re.S)
    if not m:
        fail("sys_net.c: missing struct socket_entry")
    if "struct net_tcp_pcb tcp;" not in m.group(1):
        fail("socket_entry does not carry a per-socket TCP PCB")
    if sys_net.count("struct net_tcp_pcb tcp;") != 1:
        fail("sys_net.c should not copy registered TCP PCBs into local structs")
    if "s->tcp =" in sys_net:
        fail("sys_net.c should not assign/copy registered TCP PCB structs")

    if re.search(r"\bnet_tcp_(connect|send|recv|close)\s*\(", sys_net):
        fail("sys_net.c stream path should use net_tcp_*_pcb APIs")

    required = [
        "net_tcp_connect_pcb",
        "net_tcp_send_pcb",
        "net_tcp_recv_pcb",
        "net_tcp_close_pcb",
    ]
    for name in required:
        if name not in sys_net:
            fail(f"sys_net.c does not reference {name}")

    if "static struct net_tcp_pcb legacy_tcp;" not in net_c:
        fail("net.c should keep legacy TCP wrappers on top of a PCB")
    if re.search(r"static\s+struct\s*\{.*?\}\s*tcp\s*;", net_c, re.S):
        fail("net.c still defines the old global anonymous TCP state")

    for snippet in [
        "NET_TCP_RX_CAP",
        "registered",
        "snd_una",
        "peer_window",
        "rx_len",
        "rx_buf[NET_TCP_RX_CAP]",
        "struct net_tcp_pcb *next",
    ]:
        if snippet not in net_h:
            fail(f"net_tcp_pcb is missing {snippet}")

    for snippet in [
        "dev_recv_raw",
        "net_tcp_dispatch_frame",
        "net_tcp_register_pcb",
        "net_tcp_unregister_pcb",
        "net_tcp_queue_rx",
        "net_tcp_take_rx",
        "net_tcp_rx_buffered",
        "net_tcp_rx_dropped",
        "net_tcp_receive_window",
        "NET_TCP_RETRIES",
        "accepted_fin",
        "net_tcp_dispatch_frame(rxbuf, n)",
    ]:
        if snippet not in net_c:
            fail(f"net.c is missing TCP demux/buffer primitive: {snippet}")

    for snippet in [
        "wget <host> [port]",
        "tcptwotest <host> <p1> <p2>",
        "cmd_tcptwotest",
        "parse_ipv4(host, &ip)",
        "addr.sin_port = htons((uint16_t)port)",
    ]:
        if snippet not in shell_c:
            fail(f"shell wget command is missing TCP socket test support: {snippet}")

    for snippet in [
        "Start-TcpSmokeServer",
        "Wait-TcpSmokeServer",
        "BUZZOS_TCP_SMOKE_OK",
        "BUZZOS_TCP_TWO_A",
        "BUZZOS_TCP_TWO_B",
        "tcptwotest 10.0.2.2",
        "wget 10.0.2.2",
        '"x" * 3072',
    ]:
        if snippet not in smoke_ps1:
            fail(f"smoke.ps1 is missing deterministic TCP socket coverage: {snippet}")

    ok("network sockets: stream sockets use per-socket TCP PCBs with rx demux buffers and single/dual smoke TCP coverage")


def check_procfs_diagnostics():
    procfs = read_text("src/kernel/fs/procfs.c")
    shell_c = read_text("src/user/bin/shell.c")
    gui_c = read_text("src/user/bin/gui.c")
    terminal_c = read_text("src/user/bin/terminal.c")
    report_py = read_text("tools/project_report.py")
    smoke_ps1 = read_text("scripts/smoke.ps1")

    for entry in ["about", "health", "interfaces", "limits", "tasks", "threads", "meminfo", "net", "sync", "fds", "mounts", "fs"]:
        if f'{{ "{entry}",' not in procfs:
            fail(f"procfs is missing /proc/{entry}")

    for snippet in [
        "PROC_NODE_ABOUT",
        "PROC_NODE_HEALTH",
        "PROC_NODE_INTERFACES",
        "PROC_NODE_LIMITS",
        "PROC_NODE_FS",
        "proc_about_text",
        "proc_health_text",
        "proc_interfaces_text",
        "proc_limits_text",
        "proc_fs_text",
        "native-x86_64-posix-os",
        "mode long64",
        "interfaces proc shell gui report",
        "NAME STATUS ENTRYPOINTS",
        "about stable /proc/about,about,gui:about,make:report",
        "limits stable /proc/limits,limits,gui:limits,make:report",
        "fs stable /fs,/proc/fs,fsinfo,fsstat,tools:check_minifs",
        "max_tasks",
        "max_fd_per_owner",
        "minifs_max_file_size",
        "mount /fs",
        "driver minifs",
        "host_repair make fs-repair",
        "gui:interfaces,make:report",
        "minifs_info(&fs)",
        "net_ip",
        "PROC_NODE_FDS",
        "proc_fds_text",
        "OWNER FD OF REFS FLAGS KIND NAME DETAIL",
        "fd_to_open_file(owner, fd)",
        "pipe=",
    ]:
        if snippet not in procfs:
            fail(f"procfs fd diagnostics missing: {snippet}")

    if "cmd_fdstat" not in shell_c or 'cmd_cat("/proc/fds")' not in shell_c:
        fail("shell is missing fdstat /proc/fds command")
    if "cmd_about" not in shell_c or 'cmd_cat("/proc/about")' not in shell_c:
        fail("shell is missing about /proc/about command")
    if "cmd_health" not in shell_c or 'cmd_cat("/proc/health")' not in shell_c:
        fail("shell is missing health /proc/health command")
    if "cmd_interfaces" not in shell_c or 'cmd_cat("/proc/interfaces")' not in shell_c:
        fail("shell is missing interfaces /proc/interfaces command")
    if "cmd_limits" not in shell_c or 'cmd_cat("/proc/limits")' not in shell_c:
        fail("shell is missing limits /proc/limits command")
    if "cmd_fsinfo" not in shell_c or 'cmd_cat("/proc/fs")' not in shell_c:
        fail("shell is missing fsinfo /proc/fs command")
    # Terminal moved out of the desktop into its own GUI app, so these are
    # checked against terminal.c rather than gui.c.  The invariant is that the
    # terminal drives the real /bin/sh over inherited stdio, not a reduced
    # built-in command set.
    for snippet in [
        'shell_argv[] = {"/bin/sh"}',
        'spawn_process_args("/bin/sh"',
        "SPAWN_FLAG_INHERIT_STDIO",
    ]:
        if snippet not in terminal_c:
            fail(f"GUI terminal is not attached to the full /bin/sh command surface: {snippet}")
    if "collect_health_interfaces" not in report_py or "/proc/health" not in report_py:
        fail("project report is missing health interface summary")
    if "collect_project_identity" not in report_py or "/proc/about" not in report_py:
        fail("project report is missing project identity summary")
    if "collect_runtime_interfaces" not in report_py or "/proc/interfaces" not in report_py:
        fail("project report is missing runtime interface summary")
    if "collect_runtime_limits" not in report_py or "/proc/limits" not in report_py:
        fail("project report is missing runtime limits summary")
    if "collect_fs_interfaces" not in report_py or "/proc/fs" not in report_py:
        fail("project report is missing filesystem interface summary")
    for snippet in [
        "cat /proc/about",
        "cat /proc/health",
        "cat /proc/interfaces",
        "cat /proc/limits",
        "cat /proc/fs",
        "about",
        "health",
        "interfaces",
        "limits",
        "fsinfo",
        "name\\s+BuzzOS",
        "native-x86_64-posix-os",
        "mode\\s+long64",
        "status\\s+ok",
        "interfaces\\s+proc\\s+shell\\s+gui\\s+report",
        "proc_entries\\s+12",
        "NAME\\s+STATUS\\s+ENTRYPOINTS",
        "about\\s+stable\\s+/proc/about,about,gui:about,make:report",
        "limits\\s+stable\\s+/proc/limits,limits,gui:limits,make:report",
        "fs\\s+stable\\s+/fs,/proc/fs,fsinfo,fsstat,tools:check_minifs",
        "max_tasks\\s+32",
        "max_fd_per_owner\\s+32",
        "minifs_max_file_size\\s+32441856",
        "mount\\s+/fs",
        "driver\\s+minifs",
        "inodes_total\\s+2048",
        "blocks_total\\s+63363",
        "host_repair\\s+make fs-repair",
        "gui:interfaces,make:report",
        "fs_status\\s+ok",
        "cat /proc/fds",
        "fdstat",
        "OWNER\\s+FD\\s+OF\\s+REFS",
    ]:
        if snippet not in smoke_ps1:
            fail(f"smoke.ps1 is missing procfs diagnostics coverage: {snippet}")

    ok("procfs diagnostics: /proc/about, /proc/health, /proc/interfaces, /proc/limits, /proc/fs, full shell/GUI terminal access, and fdstat are covered")


def check_host_doctor():
    makefile = read_text("Makefile")
    doctor = read_text("tools/doctor.py")
    workflow = read_text("tools/workflow.py")
    report_py = read_text("tools/project_report.py")
    readme = read_text("README.md")
    readme_en = read_text("README.en.md")
    docs_readme = read_text("docs/README.md")
    boot_guide = read_text("docs/boot-guide.md")
    user_guide = read_text("docs/user-guide.md")
    minifs_doc = read_text("docs/minifs.md")
    work_items = read_text("docs/work-items.md")
    changelog = read_text("CHANGELOG.md")

    for snippet in [
        ".PHONY: all clean help doctor",
        "help:",
        "tools/workflow.py",
        "doctor:",
        "tools/doctor.py --python",
        "--qemu \"$(QEMU)\"",
        "run-gui:",
        "-Command gui",
        "FS_REPAIR_IMAGE",
        "fs-repair:",
        "tools/check_minifs.py --image \"$(FS_IMAGE)\" --repair --out \"$(FS_REPAIR_IMAGE)\"",
    ]:
        if snippet not in makefile:
            fail(f"Makefile is missing host doctor wiring: {snippet}")

    for snippet in [
        "def check_tool",
        "def check_powershell",
        "def check_workspace",
        "--soft",
        "--no-version",
        "qemu-system-x86_64",
        "llvm-objcopy",
        "scripts/run-local.ps1",
    ]:
        if snippet not in doctor:
            fail(f"tools/doctor.py is missing expected host check: {snippet}")

    for snippet in [
        "WORKFLOW =",
        "make doctor",
        "make run-local",
        "make run-gui",
        "make smoke",
        "make gui-smoke",
        "make verify",
        "make report",
        "make fs-repair",
        "make image-reset-fs",
        "--markdown",
    ]:
        if snippet not in workflow:
            fail(f"tools/workflow.py is missing expected local workflow item: {snippet}")

    for snippet in [
        "collect_host_doctor",
        "collect_local_workflow",
        "collect_guide_docs",
        "collect_fs_interfaces",
        "## Host Doctor",
        "## Local Workflow",
        "## Guide Docs",
        "## Filesystem Interfaces",
        "tools/doctor.py",
        "make help",
        "make run-gui",
        "make fs-repair",
        "--soft",
        "--no-version",
        "make doctor",
    ]:
        if snippet not in report_py:
            fail(f"project report is missing host doctor summary: {snippet}")

    for snippet in ["make help", "make doctor", "QEMU=", "tools/doctor.py", "make run-gui", "make fs-repair", "docs/boot-guide.md", "docs/user-guide.md"]:
        if snippet not in readme or snippet not in readme_en:
            fail(f"README files are missing host doctor guidance: {snippet}")

    for snippet in ["boot-guide.md", "user-guide.md", "本地启动与引导指南", "用户指南"]:
        if snippet not in docs_readme:
            fail(f"docs/README.md is missing guide link: {snippet}")

    for snippet in ["make doctor", "make run-local", "Ctrl+Alt+G", "make run-gui", "No rule to make target 'smoke'", "fsinfo", "cat /proc/fs"]:
        if snippet not in boot_guide:
            fail(f"docs/boot-guide.md is missing local startup guidance: {snippet}")

    for snippet in ["help proc", "gui", "textedit", "paint", "calculator", "fsinfo", "cat /proc/fs", "/proc", "/fs", "输入框", "nano"]:
        if snippet not in user_guide:
            fail(f"docs/user-guide.md is missing user guidance: {snippet}")

    for snippet in ["make fs-repair", "FS_REPAIR_IMAGE", "--repair --out"]:
        if snippet not in minifs_doc:
            fail(f"docs/minifs.md is missing fs repair guidance: {snippet}")
    for snippet in ["Done/P2: minifs", "make fs-repair", "tools/check_minifs_repair.py"]:
        if snippet not in work_items:
            fail(f"docs/work-items.md is missing minifs repair completion: {snippet}")

    for snippet in ["make help", "make doctor", "make run-gui", "make fs-repair"]:
        if snippet not in changelog:
            fail(f"CHANGELOG is missing workflow entry: {snippet}")

    ok("host workflow: make help, doctor, report summary, and docs are covered")


def check_futex_blocking():
    sys_ipc = read_text("src/kernel/syscall/sys_ipc.c")
    sys_ipc_h = read_text("src/kernel/syscall/sys_ipc.h")
    task_c = read_text("src/kernel/sched/task.c")
    task_h = read_text("src/kernel/sched/task.h")
    procfs = read_text("src/kernel/fs/procfs.c")
    shell_c = read_text("src/user/bin/shell.c")
    smoke_ps1 = read_text("scripts/smoke.ps1")

    for snippet in [
        "struct futex_waiter",
        "futex_waiters[MAX_FUTEX_WAITERS]",
        "task_block_current();",
        "task_block_current_until(deadline);",
        "task_wake(futex_waiters[i].task_id)",
        "futex_cancel_task_locked",
        "futex_status_text",
        "SLOT TID ADDR       WOKEN",
    ]:
        if snippet not in sys_ipc:
            fail(f"futex scheduler-backed wait support is missing: {snippet}")

    if "void futex_cancel_task_locked(int task_id);" not in sys_ipc_h:
        fail("sys_ipc.h does not expose futex_cancel_task_locked")

    for snippet in [
        "futex_cancel_task_locked(current_task->id)",
        "current_task->state = TASK_BLOCKED",
        "void task_block_current(void)",
        "void task_block_current_until(uint32_t wake_tick)",
        "int task_wake(int id)",
    ]:
        if snippet not in task_c + "\n" + task_h:
            fail(f"scheduler futex integration is missing: {snippet}")

    for snippet in [
        "PROC_NODE_SYNC",
        "futex_status_text",
        "TASK_BLOCKED",
        "BLOCKED",
    ]:
        if snippet not in procfs + "\n" + task_c:
            fail(f"proc/scheduler diagnostics are missing futex blocking visibility: {snippet}")

    for snippet in [
        "cmd_futexblocktest",
        "futex_wait(&futex_block_word, 0)",
        "cmd_threads()",
        "cmd_syncstat()",
        "futexblock: waiting threads",
    ]:
        if snippet not in shell_c:
            fail(f"shell is missing futex blocking coverage: {snippet}")

    for snippet in [
        "futexblocktest",
        "futexblock: waiting threads",
        "BLOCKED\\s+tty\\s+user_thread",
        "futex_waiters\\s+1/32",
        "futexblock: woke\\s+1",
        "futexcancel: killed 34 wait -2",
    ]:
        if snippet not in smoke_ps1:
            fail(f"smoke.ps1 is missing futex blocking coverage: {snippet}")

    ok("futex: waits block in the scheduler, wake by address, and smoke covers blocked visibility/cancel cleanup")


def check_shell_pipeline():
    vfs_h = read_text("src/kernel/fs/vfs.h")
    vfs_c = read_text("src/kernel/fs/vfs.c")
    exec_c = read_text("src/kernel/core/exec.c")
    sys_proc = read_text("src/kernel/syscall/sys_proc.c")
    libc_h = read_text("src/user/libc/libc.h")
    shell_c = read_text("src/user/bin/shell.c")
    makefile = read_text("Makefile")
    kernel_c = read_text("src/kernel/core/kernel.c")
    smoke_ps1 = read_text("scripts/smoke.ps1")

    for path in ["src/user/bin/cat.c", "src/user/bin/echo.c"]:
        if not (ROOT / path).exists():
            fail(f"missing pipeline user program: {path}")

    for snippet in [
        "int  vfs_clone_fd_table(int dst_task_id, int src_task_id)",
        "int vfs_clone_fd_table(int dst_task_id, int src_task_id)",
        "int  vfs_clone_stdio(int dst_task_id, int src_task_id)",
        "int vfs_clone_stdio(int dst_task_id, int src_task_id)",
        "clone_fd_range(dst_task_id, src_task_id, 3)",
        "p->readers++",
        "p->writers++",
        "src->vnode.data == &src->stream",
    ]:
        source = vfs_h + "\n" + vfs_c
        if snippet not in source:
            fail(f"VFS fd inheritance is missing: {snippet}")

    for snippet in [
        "exec_start_args_with_fds",
        "vfs_clone_fd_table(id, inherit_fd_owner)",
        "vfs_clone_stdio(id, inherit_fd_owner)",
        "inherit_fd_owner >= 0",
        "inherit_stdio_only",
    ]:
        if snippet not in exec_c:
            fail(f"exec fd inheritance path is missing: {snippet}")

    for snippet in [
        "flags & 2u",
        "flags & 4u",
        "current_fd_owner()",
        "exec_start_file_args_with_fds",
        "inherit_stdio && !inherit_all",
    ]:
        if snippet not in sys_proc:
            fail(f"spawn fd inheritance flag is missing: {snippet}")

    if "SPAWN_FLAG_INHERIT_FDS 2" not in libc_h:
        fail("user libc is missing SPAWN_FLAG_INHERIT_FDS")
    if "SPAWN_FLAG_INHERIT_STDIO 4" not in libc_h:
        fail("user libc is missing SPAWN_FLAG_INHERIT_STDIO")

    for snippet in [
        "execute_pipeline",
        "line[i] == '|'",
        "PIPELINE_MAX 6",
        "struct pipeline_cmd",
        "parse_pipeline_cmd",
        "has_exec_operator",
        "pipe(fds)",
        "cmds[i].append ? O_APPEND : O_TRUNC",
        "SPAWN_FLAG_INHERIT_STDIO",
        "echo hello | cat",
        "cat < /fs/out",
    ]:
        if snippet not in shell_c:
            fail(f"shell pipeline support is missing: {snippet}")

    for snippet in [
        "CAT_ELF",
        "ECHO_ELF",
        "/bin/cat $(CAT_ELF)",
        "/bin/echo $(ECHO_ELF)",
        "src/user/bin/cat.c",
        "src/user/bin/echo.c",
    ]:
        if snippet not in makefile:
            fail(f"Makefile is missing pipeline user program wiring: {snippet}")

    for snippet in [
        'ramfs_register("/bin/cat"',
        'ramfs_register("/bin/echo"',
    ]:
        if snippet not in kernel_c:
            fail(f"kernel initrd registration is missing: {snippet}")

    for snippet in [
        "help pipes",
        '"|" { return "shift-backslash" }',
        '">" { return "shift-dot" }',
        '"<" { return "shift-comma" }',
        "echo pipelinesmokeok | cat",
        "echo multipipesmokeok | cat | cat",
        "echo redir-one > /fs/redir",
        "echo redir-two >> /fs/redir",
        "cat < /fs/redir",
        "pipelinesmokeok",
        "multipipesmokeok",
        "\\[pipe\\] exited 0 \\| 0",
        "\\[pipe\\] exited 0 \\| 0 \\| 0",
        "redir-one",
        "redir-two",
    ]:
        if snippet not in smoke_ps1:
            fail(f"smoke.ps1 is missing shell pipeline coverage: {snippet}")

    ok("shell pipeline: stdio-only inheritance supports multi-stage pipes and redirection in smoke")


def check_pipe_blocking():
    vfs_internal = read_text("src/kernel/fs/vfs_internal.h")
    pipefs_c = read_text("src/kernel/fs/pipefs.c")
    task_h = read_text("src/kernel/sched/task.h")
    task_c = read_text("src/kernel/sched/task.c")
    shell_c = read_text("src/user/bin/shell.c")
    smoke_ps1 = read_text("scripts/smoke.ps1")

    for snippet in [
        "uint32_t read_waiters;",
        "uint32_t write_waiters;",
    ]:
        if snippet not in vfs_internal:
            fail(f"pipe object is missing waiter state: {snippet}")

    for snippet in [
        "void task_prepare_block_current(uint32_t wake_tick)",
        "current_task->state = TASK_BLOCKED",
        "task_prepare_block_current(0)",
        "task_prepare_block_current(wake_tick ? wake_tick : 1)",
    ]:
        if snippet not in task_h + "\n" + task_c:
            fail(f"scheduler is missing reusable task blocking primitive: {snippet}")

    for snippet in [
        "static int pipe_wait(struct pipe_obj *p, int end)",
        "static void pipe_wake_mask(uint32_t *mask)",
        "task_prepare_block_current(0)",
        "vfs_unlock();",
        "task_yield();",
        "vfs_lock();",
        "p->read_waiters |= bit",
        "p->write_waiters |= bit",
        "pipe_wake_mask(&p->write_waiters)",
        "pipe_wake_mask(&p->read_waiters)",
        "while (p->count == 0 && p->writers > 0)",
        "while (p->count == PIPE_BUFSZ && p->readers > 0)",
    ]:
        if snippet not in pipefs_c:
            fail(f"pipefs blocking/wake support is missing: {snippet}")

    if re.search(r"if\s*\(\s*p->count\s*==\s*0\s*&&\s*p->writers\s*>\s*0\s*\)\s*return\s+-1\s*;", pipefs_c):
        fail("pipe_read still returns -1 for an empty pipe with live writers")
    if re.search(r"if\s*\(\s*p->count\s*==\s*PIPE_BUFSZ\s*&&\s*p->readers\s*>\s*0\s*\)\s*return\s+-1\s*;", pipefs_c):
        fail("pipe_write still returns -1 for a full pipe with live readers")

    for snippet in [
        "cmd_pipeblocktest",
        "pipe_block_reader_thread",
        "pipe_block_writer_thread",
        "pipeblock: reader %d write %d %s",
        "pipeblock: writer %d drain %d+%d",
    ]:
        if snippet not in shell_c:
            fail(f"shell is missing pipe blocking coverage: {snippet}")

    for snippet in [
        "pipeblocktest",
        "pipeblock: reader 4 write 4 wake",
        "pipeblock: writer 600 drain 512\\+88",
    ]:
        if snippet not in smoke_ps1:
            fail(f"smoke.ps1 is missing pipe blocking coverage: {snippet}")

    ok("pipes: reads/writes block on empty/full buffers and smoke covers wakeups")


def decode_c_strings(block):
    parts = re.findall(r'"((?:\\.|[^"\\])*)"', block)
    out = []
    for part in parts:
        out.append(bytes(part, "utf-8").decode("unicode_escape"))
    return "".join(out)


def parse_manifest_block(source, symbol):
    m = re.search(
        rf"static\s+const\s+char\s+{re.escape(symbol)}_manifest\[\]\s*=\s*(.*?);",
        source,
        re.S,
    )
    if not m:
        fail(f"missing {symbol}_manifest in app registry sources")
    text = decode_c_strings(m.group(1))
    result = {}
    for line in text.splitlines():
        if "=" in line:
            key, value = line.split("=", 1)
            result[key] = value
    return result


def parse_manifest_text(text):
    result = {}
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if "=" in line:
            key, value = line.split("=", 1)
            result[key.strip()] = value.strip()
    return result


def discover_app_manifests(source):
    apps = []
    seen = set()
    for m in re.finditer(
        r"static\s+const\s+char\s+([A-Za-z_][A-Za-z0-9_]*)_manifest\[\]\s*=",
        source,
    ):
        symbol = m.group(1)
        manifest = parse_manifest_block(source, symbol)
        exec_path = manifest.get("exec", "")
        if not exec_path.startswith("/fs/apps/"):
            continue
        app = exec_path.rsplit("/", 1)[-1]
        if app in seen:
            fail(f"duplicate app manifest for {app}")
        seen.add(app)
        apps.append((app, symbol, manifest))
    if not apps:
        fail("no /fs/apps manifests found in app registry sources")
    return apps


def check_app_manifests(list_only=False):
    kernel_c = read_text("src/kernel/core/kernel.c")
    app_registry = read_text_if_exists("build/generated/app_registry.h")
    source = kernel_c + "\n" + app_registry
    makefile = read_text("Makefile")
    app_names = set(parse_make_words(makefile, "GUI_APP_NAMES"))
    apps = discover_app_manifests(source)
    discovered = {app for app, _symbol, _manifest in apps}

    missing_from_registry = sorted(app_names - discovered)
    extra_in_registry = sorted(discovered - app_names)
    if missing_from_registry:
        fail("GUI_APP_NAMES missing from generated app registry: " + ", ".join(missing_from_registry))
    if extra_in_registry:
        fail("generated app registry has apps outside GUI_APP_NAMES: " + ", ".join(extra_in_registry))

    if list_only:
        print("app       version  kind      exec")
        print("--------  -------  --------  ----------------")
        for app, _symbol, manifest in apps:
            print(f"{app:<8}  {manifest.get('version', '-'):<7}  {manifest.get('kind', '-'):<8}  {manifest.get('exec', '-')}")
        return

    required = ["name", "kind", "version", "summary", "exec", "state", "source"]
    seeded_manifest_paths = set(re.findall(r'"/fs/apps/([A-Za-z0-9_]+)\.app"', source))
    seeded_exec_paths = set(re.findall(r'"/fs/apps/([A-Za-z0-9_]+)"', source))

    for app, symbol, manifest in apps:
        missing = [key for key in required if key not in manifest or not manifest[key]]
        if missing:
            fail(f"{app}.app manifest missing keys: {', '.join(missing)}")
        if symbol not in (app, f"app_{app}"):
            fail(f"{app}.app manifest symbol should be {app}_manifest, got {symbol}_manifest")
        if manifest["exec"] != f"/fs/apps/{app}":
            fail(f"{app}.app exec should be /fs/apps/{app}")
        if not manifest["state"].startswith("/fs/"):
            fail(f"{app}.app state should live on persistent /fs storage")
        if app not in app_names:
            fail(f"{app} is missing from GUI_APP_NAMES in Makefile")
        source = ROOT / manifest["source"]
        if not source.exists():
            fail(f"{app}.app source does not exist: {manifest['source']}")
        if not (ROOT / f"build/user/{app}.elf").exists():
            fail(f"missing build/user/{app}.elf")
        if app not in seeded_manifest_paths:
            fail(f"kernel does not seed /fs/apps/{app}.app")
        if app not in seeded_exec_paths:
            fail(f"kernel does not seed /fs/apps/{app}")
        sidecar = ROOT / f"src/user/bin/{app}.app"
        if not sidecar.exists():
            fail(f"missing source manifest sidecar: src/user/bin/{app}.app")
        sidecar_manifest = parse_manifest_text(sidecar.read_text(encoding="utf-8"))
        for key in required:
            if sidecar_manifest.get(key) != manifest.get(key):
                fail(f"{app}.app generated registry is out of sync for key {key}")
    ok("app manifests: " + ", ".join(app for app, _symbol, _manifest in apps))


def check_gui_style():
    makefile = read_text("Makefile")
    appui_h = read_text("src/user/libc/appui.h")
    guiapp_h = read_text("src/user/libc/guiapp.h")
    new_app_py = read_text("tools/new_app.py")
    report_py = read_text("tools/project_report.py")
    readme = read_text("README.md")
    readme_en = read_text("README.en.md")
    project_status = read_text("docs/project-status.md")
    changelog = read_text("CHANGELOG.md")

    for snippet in [
        "USER_HEADERS",
        "src/user/libc/appui.h",
        "src/user/libc/guiapp.h",
        "$(BUILD)/user/%.o: src/user/bin/%.c $(USER_HEADERS)",
    ]:
        if snippet not in makefile:
            fail(f"Makefile is missing desktop app dependency: {snippet}")

    for snippet in [
        "appui_rect",
        "appui_fill",
        "appui_border",
        "appui_text",
        "appui_button",
    ]:
        if snippet not in appui_h:
            fail(f"appui.h is missing shared drawing helper: {snippet}")
    for snippet in ["appui_utf8_next", "appui_utf8_prev", "appui_draw_codepoint", "font_glyph"]:
        if snippet not in appui_h:
            fail(f"appui.h is missing UTF-8 drawing feature: {snippet}")
    for snippet in ["guiapp_event", "GUIAPP_EVT_CLOSE", "guiapp_send_frame", "guiapp_send_dirty"]:
        if snippet not in guiapp_h:
            fail(f"guiapp.h is missing desktop protocol feature: {snippet}")
    for snippet in ["GUIAPP_FRAME_LAUNCH", "GUIAPP_PATH_MAX", "guiapp_request_launch"]:
        if snippet not in guiapp_h:
            fail(f"guiapp.h is missing cross-app launch feature: {snippet}")
    gui_c = read_text("src/user/bin/gui.c")
    terminal_c = read_text("src/user/bin/terminal.c")
    textedit_c = read_text("src/user/bin/textedit.c")
    files_c = read_text("src/user/bin/filemanager.c")
    pinyin_h = read_text("src/user/bin/pinyin_data.h")
    shell_c = read_text("src/user/bin/shell.c")
    for snippet in ["GUIAPP_EVT_TEXT", "GUIAPP_TEXT_MAX"]:
        if snippet not in guiapp_h:
            fail(f"GUI app protocol is missing system text input: {snippet}")
    for snippet in ["GUIAPP_EVT_COMMAND", "GUIAPP_FRAME_CLIPBOARD", "guiapp_set_clipboard"]:
        if snippet not in guiapp_h + read_text("src/user/libc/guiapp.c"):
            fail(f"GUI app protocol is missing system clipboard support: {snippet}")
    for snippet in ["ime_handle_key", "ime_submit", "draw_ime", "app_send_text"]:
        if snippet not in gui_c:
            fail(f"desktop is missing system Pinyin IME feature: {snippet}")
    for snippet in ["draw_context_menu", "clipboard_command", "GUIAPP_CMD_COPY", "GUIAPP_CMD_CUT"]:
        if snippet not in gui_c:
            fail(f"desktop is missing context clipboard feature: {snippet}")
    for snippet in ["input_line", "track_input_text", 'send_shell("\\x15"']:
        if snippet not in terminal_c:
            fail(f"desktop Terminal is missing UTF-8 clipboard input tracking: {snippet}")
    for snippet in ["position_from_mouse_locked", "selection_exists_locked", "copy_selection_locked", "selection_dragging"]:
        if snippet not in terminal_c:
            fail(f"desktop Terminal is missing visible mouse selection: {snippet}")
    for snippet in ["utf8_prev", "utf8_next", "c <= 255", "c == 0x15"]:
        if snippet not in shell_c:
            fail(f"shell is missing UTF-8 line editing support: {snippet}")
    for snippet in ['{"ni",', '{"zhongguo",', "PINYIN_ENTRY_COUNT"]:
        if snippet not in pinyin_h:
            fail(f"Pinyin dictionary is missing coverage: {snippet}")
    for snippet in ["app_target_allowed", "run_app_with_arg", "GUIAPP_FRAME_LAUNCH"]:
        if snippet not in gui_c:
            fail(f"desktop is missing cross-app launch handling: {snippet}")
    for snippet in ["MAX_GUI_APPS = 10", "taskbar_expanded", "collect_open_apps",
                    "draw_taskbar_tooltip", "activate_next_visible"]:
        if snippet not in gui_c:
            fail(f"desktop is missing scalable task switcher feature: {snippet}")
    for snippet in ["set_document_path", "argc > 4", "file_path"]:
        if snippet not in textedit_c:
            fail(f"TextEdit is missing document argument support: {snippet}")
    if "GUIAPP_EVT_TEXT" not in textedit_c or "insert_text" not in textedit_c:
        fail("TextEdit is missing system UTF-8 text input support")
    for snippet in ["selection_anchor", "position_at", "guiapp_set_clipboard"]:
        if snippet not in textedit_c:
            fail(f"TextEdit is missing selection/clipboard support: {snippet}")
    for snippet in ["getdents", "guiapp_request_launch", "MODE_DELETE", "scan_directory"]:
        if snippet not in files_c:
            fail(f"filemanager is missing required feature: {snippet}")
    for snippet in ["is_elf_file", "has_gui_manifest", "guiapp_request_exec"]:
        if snippet not in files_c:
            fail(f"filemanager is missing CLI/GUI executable dispatch: {snippet}")
    for snippet in ["GUIAPP_FRAME_EXEC", 'run_app_with_arg("/fs/apps/terminal"', "exec_target_allowed"]:
        if snippet not in guiapp_h + gui_c:
            fail(f"desktop is missing non-GUI ELF terminal dispatch: {snippet}")
    for snippet in ["app_reader_loop", "app_reader_functions", "reader_dead", "reap_dead_apps"]:
        if snippet not in gui_c:
            fail(f"desktop is missing asynchronous app frame handling: {snippet}")

    for app in parse_make_words(makefile, "GUI_APP_NAMES"):
        source = read_text(f"src/user/bin/{app}.c")
        for snippet in ['#include "appui.h"', '#include "guiapp.h"', "guiapp_read_event", "guiapp_send_frame"]:
            if snippet not in source:
                fail(f"{app}.c is missing current desktop app feature: {snippet}")

    for snippet in ["collect_gui_style", "## GUI Style", "appui.h"]:
        if snippet not in report_py:
            fail(f"project report is missing desktop UI summary: {snippet}")

    for snippet in ['#include "appui.h"', '#include "guiapp.h"', "guiapp_parse_args", "guiapp_read_event", "guiapp_send_frame"]:
        if snippet not in new_app_py:
            fail(f"new app scaffold is missing current desktop protocol: {snippet}")

    docs = readme + "\n" + readme_en + "\n" + project_status + "\n" + changelog
    for snippet in ["appui.h", "guiapp.h"]:
        if snippet not in docs:
            fail(f"docs/logs are missing desktop app helper note: {snippet}")

    ok("desktop apps: seeded apps and scaffolds use shared drawing helpers and the current event/frame protocol")


def main():
    parser = argparse.ArgumentParser(description="BuzzOS host-side consistency checks")
    parser.add_argument("--apps-only", action="store_true", help="only validate /fs/apps manifests and build outputs")
    parser.add_argument("--list-apps", action="store_true", help="list kernel-seeded /fs/apps manifests")
    parser.add_argument("--source-only", action="store_true", help="run checks that do not require build artifacts")
    args = parser.parse_args()

    if args.list_apps:
        check_app_manifests(list_only=True)
        return
    if args.apps_only:
        check_app_manifests()
        print("App check passed.")
        return
    if args.source_only:
        check_user_bounds()
        check_user_fault_isolation()
        check_runtime_lifecycle()
        check_elf_loader_hardening()
        check_dynamic_heap()
        check_syscall_abi()
        check_network_socket_state()
        check_procfs_diagnostics()
        check_host_doctor()
        check_futex_blocking()
        check_shell_pipeline()
        check_pipe_blocking()
        check_gui_style()
        print("Source check passed.")
        return

    check_image_layout()
    check_kernel_memory_layout()
    load_start, load_end, stack_top = check_user_bounds()
    check_user_fault_isolation()
    check_runtime_lifecycle()
    check_elf_loader_hardening()
    check_dynamic_heap()
    check_syscall_abi()
    check_network_socket_state()
    check_procfs_diagnostics()
    check_host_doctor()
    check_futex_blocking()
    check_shell_pipeline()
    check_pipe_blocking()
    check_user_elves(load_start, load_end, stack_top)
    check_initrd_reachability()
    check_initrd_hygiene()
    check_app_manifests()
    check_gui_style()
    print("Project check passed.")


if __name__ == "__main__":
    main()
