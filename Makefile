BUILD := build
GENERATED_DIR := $(BUILD)/generated
OBJDIR := $(BUILD)/obj/kernel
IMAGE := $(BUILD)/buzzos.img
BOOT_PARTITION_START := 2048
BOOT_PARTITION_SECTORS := 65536
FS_START_SECTOR := 67584
FS_SECTORS := 32768
FS_IMAGE ?= $(IMAGE)
FS_TEST_IMAGE ?= $(BUILD)/buzzos-test.img
FS_REPAIR_IMAGE ?= $(BUILD)/buzzos-repaired.img

# Keep MSYS/Git shells from rewriting initrd virtual paths such as /bin/sh
# into Windows host paths when invoking native Python.
export MSYS2_ARG_CONV_EXCL := *
export MSYS_NO_PATHCONV := 1

KERNEL_SRCS := \
	src/kernel/core/kernel.c \
	src/kernel/core/elf.c \
	src/kernel/core/exec.c \
	src/kernel/arch/i386/gdt.c \
	src/kernel/arch/i386/idt.c \
	src/kernel/arch/i386/paging.c \
	src/kernel/arch/i386/fpu.c \
	src/kernel/arch/i386/user.c \
	src/kernel/mm/pmm.c \
	src/kernel/sched/task.c \
	src/kernel/syscall/syscall.c \
	src/kernel/syscall/sys_file.c \
	src/kernel/syscall/sys_proc.c \
	src/kernel/syscall/sys_net.c \
	src/kernel/syscall/sys_ipc.c \
	src/kernel/syscall/sys_shm.c \
	src/kernel/syscall/sys_audio.c \
	src/kernel/syscall/sys_gfx.c \
	src/kernel/fs/vfs.c \
	src/kernel/fs/ramfs.c \
	src/kernel/fs/devfs.c \
	src/kernel/fs/procfs.c \
	src/kernel/fs/pipefs.c \
	src/kernel/fs/minifs_vfs.c \
	src/kernel/block/ata.c \
	src/kernel/block/cache.c \
	src/kernel/fs/minifs/minifs.c \
	src/kernel/net/netdev.c \
	src/kernel/net/net.c \
	src/kernel/drv/keyboard.c \
	src/kernel/drv/mouse.c \
	src/kernel/drv/timer.c \
	src/kernel/drv/rtc.c \
	src/kernel/drv/serial.c \
	src/kernel/drv/ac97.c \
	src/kernel/drv/font_unicode.c \
	src/kernel/drv/fb.c \
	src/kernel/drv/reboot.c \
	src/kernel/drv/ne2000.c

KERNEL_ASMS := \
	src/kernel/arch/i386/mb2_entry.asm \
	src/kernel/arch/i386/isr.asm \
	src/kernel/arch/i386/switch.asm \
	src/kernel/arch/i386/setjmp.asm

KERNEL_C_OBJS  := $(patsubst src/kernel/%.c,$(OBJDIR)/%.o,$(KERNEL_SRCS))
KERNEL_ASM_OBJS:= $(patsubst src/kernel/%.asm,$(OBJDIR)/%.o-asm,$(KERNEL_ASMS))
KERNEL_OBJS    := $(KERNEL_C_OBJS) $(KERNEL_ASM_OBJS)

NASM := nasm
CC   := clang
LD   := ld.lld
OBJCOPY := llvm-objcopy
PYTHON ?= python
QEMU ?= qemu-system-i386
LIMINE_DIR ?= D:/limine-binary/limine-binary

KERNEL_INCLUDES := \
	-Isrc/kernel \
	-Isrc/kernel/core \
	-Isrc/kernel/arch/i386 \
	-Isrc/kernel/mm \
	-Isrc/kernel/sched \
	-Isrc/kernel/syscall \
	-Isrc/kernel/fs \
	-Isrc/kernel/fs/minifs \
	-Isrc/kernel/block \
	-Isrc/kernel/net \
	-Isrc/kernel/drv \
	-I$(GENERATED_DIR)

CFLAGS  := --target=i386-none-elf -std=c11 -ffreestanding -fno-builtin \
	-fno-stack-protector -fno-pic -mno-sse -mno-mmx -O2 -Wall -Wextra \
	$(KERNEL_INCLUDES)

# User programs may use x87 and SSE; the scheduler preserves full FXSAVE state.
UCFLAGS := --target=i386-none-elf -std=c11 -ffreestanding -fno-builtin \
	-fno-stack-protector -fno-pic -mno-sse -mno-mmx -mfpmath=387 -O2 \
	-Wall -Wextra -Isrc/user/libc
LDFLAGS := -m elf_i386 -T linker.ld -nostdlib

# User programs (linked with crt0 + libc)
USER_ELF := $(BUILD)/user/hello.elf
SHELL_ELF := $(BUILD)/user/shell.elf
NANO_ELF := $(BUILD)/user/nano.elf
BASM_ELF := $(BUILD)/user/basm.elf
BCC_ELF := $(BUILD)/user/bcc.elf
GUI_ELF := $(BUILD)/user/gui.elf
FUTEXHOLD_ELF := $(BUILD)/user/futexhold.elf
CAT_ELF := $(BUILD)/user/cat.elf
ECHO_ELF := $(BUILD)/user/echo.elf
FAULTTEST_ELF := $(BUILD)/user/faulttest.elf
SOCKETLEAK_ELF := $(BUILD)/user/socketleak.elf
HEAPTEST_ELF := $(BUILD)/user/heaptest.elf
AUDIOTEST_ELF := $(BUILD)/user/audiotest.elf
NSPORTTEST_ELF := $(BUILD)/user/nsporttest.elf
NETSURF_PORT_OBJ := $(BUILD)/user/netsurf_buzzos_platform.o
NSHTMLTEST_ELF := $(BUILD)/user/nshtmltest.elf
NETSURF_ELF := $(BUILD)/user/netsurf.elf
NSMONKEY_ELF := $(BUILD)/user/nsmonkey.elf
GUI_APP_NAMES := textedit paint calculator filemanager browser doom music
GUI_APP_ELFS := $(foreach app,$(GUI_APP_NAMES),$(BUILD)/user/$(app).elf)
GUI_APP_SRCS := $(foreach app,$(GUI_APP_NAMES),src/user/bin/$(app).c)
GUI_APP_OBJS := $(foreach app,$(GUI_APP_NAMES),$(BUILD)/user/$(app).o)
LODEPNG_DIR := src/user/third_party/lodepng
LODEPNG_OBJ := $(BUILD)/user/lodepng.o
LODEPNG_FLAGS := -I$(LODEPNG_DIR) -DLODEPNG_NO_COMPILE_DISK \
	-DLODEPNG_NO_COMPILE_ENCODER -DLODEPNG_NO_COMPILE_ANCILLARY_CHUNKS \
	-DLODEPNG_NO_COMPILE_ALLOCATORS
DOOM_DIR := src/user/third_party/doomgeneric/doomgeneric
DOOM_SRCS := $(wildcard $(DOOM_DIR)/*.c $(DOOM_DIR)/*.h) src/user/bin/doom.c src/user/bin/doom_audio.c tools/build-doom.ps1
DEMO_WAV := $(BUILD)/assets/buzzos-demo.wav
USER_ELFS := $(USER_ELF) $(SHELL_ELF) $(NANO_ELF) $(BASM_ELF) $(BCC_ELF) $(GUI_ELF) $(FUTEXHOLD_ELF) $(CAT_ELF) $(ECHO_ELF) $(FAULTTEST_ELF) $(SOCKETLEAK_ELF) $(HEAPTEST_ELF) $(AUDIOTEST_ELF) $(NSPORTTEST_ELF) $(NSHTMLTEST_ELF) $(NETSURF_ELF) $(GUI_APP_ELFS)
USER_SRCS := src/user/bin/hello.c src/user/bin/shell.c src/user/bin/nano.c src/user/bin/basm.c src/user/bin/bcc.c src/user/bin/gui.c src/user/bin/futexhold.c src/user/bin/cat.c src/user/bin/echo.c src/user/bin/faulttest.c src/user/bin/socketleak.c src/user/bin/heaptest.c src/user/bin/audiotest.c src/user/bin/nsporttest.c src/user/bin/nshtmltest.c $(GUI_APP_SRCS)
USER_LIB  := src/user/libc/crt0.c src/user/libc/libc.c src/user/libc/guiapp.c
USER_HEADERS := src/user/libc/libc.h src/user/libc/guiapp.h src/user/libc/appui.h src/kernel/drv/font_builtin.h
INITRD_H := $(GENERATED_DIR)/initrd.h
APP_REGISTRY_H := $(GENERATED_DIR)/app_registry.h
BASM_EXAMPLE := examples/basm-full.asm
BCC_EXAMPLE := examples/hello.c
UTF8_EXAMPLE := examples/utf8.txt
FONT_H := src/kernel/drv/font_builtin.h
UNICODE_FONT_H := src/kernel/drv/font_unicode_data.h
GUI_APP_META := $(foreach app,$(GUI_APP_NAMES),$(wildcard src/user/bin/$(app).app src/user/bin/$(app).readme src/user/bin/$(app).seed))

.PHONY: all clean help doctor run run-current run-local run-gui check-project app-check app-registry fs-check fs-ls fs-repair fs-check-smoke fs-check-negative fs-check-repair smoke gui-smoke report verify image-reset-fs new-app doom-install

# These objects are prerequisites of pattern-built user ELFs, not disposable
# intermediates. Keeping them makes source timestamp changes rebuild reliably.
.SECONDARY: $(GUI_APP_OBJS) \
	$(BUILD)/user/futexhold.o $(BUILD)/user/cat.o $(BUILD)/user/echo.o \
	$(BUILD)/user/faulttest.o $(BUILD)/user/socketleak.o $(BUILD)/user/heaptest.o \
	$(BUILD)/user/audiotest.o \
	$(BUILD)/user/nsporttest.o $(NETSURF_PORT_OBJ) \
	$(LODEPNG_OBJ)

all: $(IMAGE)

doom-install: $(IMAGE)
	powershell -NoProfile -ExecutionPolicy Bypass -File tools/fetch-doom-shareware.ps1 -Output build/downloads/doom1.wad
	$(PYTHON) tools/install_doom_wad.py --image $(IMAGE) --wad build/downloads/doom1.wad
	$(PYTHON) tools/check_minifs.py --image $(IMAGE)

help:
	$(PYTHON) tools/workflow.py

$(OBJDIR):
	powershell -NoProfile -Command "New-Item -ItemType Directory -Force '$(OBJDIR)' | Out-Null"

$(OBJDIR)/%.o: src/kernel/%.c | $(OBJDIR)
	powershell -NoProfile -Command "New-Item -ItemType Directory -Force (Split-Path '$@' -Parent) | Out-Null"
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR)/core/kernel.o: $(INITRD_H) $(APP_REGISTRY_H)
$(OBJDIR)/core/exec.o: src/kernel/arch/i386/user.h src/kernel/arch/i386/user_bounds.h
$(OBJDIR)/syscall/sys_proc.o: src/kernel/arch/i386/user.h src/kernel/syscall/syscall_internal.h src/kernel/arch/i386/user_bounds.h src/kernel/drv/timer.h
$(OBJDIR)/syscall/syscall.o: src/kernel/syscall/syscall_internal.h src/kernel/arch/i386/user_bounds.h
$(OBJDIR)/syscall/sys_net.o: src/kernel/syscall/syscall_internal.h src/kernel/arch/i386/user_bounds.h
$(OBJDIR)/syscall/sys_file.o: src/kernel/fs/minifs/minifs.h src/kernel/syscall/syscall_internal.h src/kernel/arch/i386/user_bounds.h
$(OBJDIR)/syscall/sys_gfx.o: src/kernel/syscall/syscall_internal.h src/kernel/arch/i386/user_bounds.h src/kernel/drv/font_unicode.h
$(OBJDIR)/sched/task.o: src/kernel/syscall/sys_ipc.h src/kernel/sched/task.h
$(OBJDIR)/syscall/sys_ipc.o: src/kernel/syscall/sys_ipc.h src/kernel/syscall/syscall_internal.h src/kernel/arch/i386/user_bounds.h src/kernel/sched/task.h src/kernel/drv/timer.h
$(OBJDIR)/fs/minifs/minifs.o: src/kernel/fs/minifs/minifs.h src/kernel/sched/task.h
$(OBJDIR)/fs/vfs.o: src/kernel/sched/task.h
$(OBJDIR)/block/cache.o: src/kernel/sched/task.h
$(OBJDIR)/fs/procfs.o: src/kernel/mm/pmm.h src/kernel/sched/task.h src/kernel/net/net.h src/kernel/syscall/sys_ipc.h
$(OBJDIR)/net/net.o: src/kernel/net/net.h src/kernel/net/netdev.h src/kernel/sched/task.h src/kernel/drv/timer.h
$(OBJDIR)/drv/timer.o: src/kernel/drv/timer.h
$(OBJDIR)/core/elf.o: src/kernel/core/elf.h src/kernel/arch/i386/user_bounds.h
$(OBJDIR)/arch/i386/paging.o: src/kernel/arch/i386/paging.h src/kernel/mm/pmm.h src/kernel/arch/i386/user_bounds.h
$(OBJDIR)/arch/i386/user.o: src/kernel/arch/i386/user.h src/kernel/arch/i386/user_bounds.h
$(OBJDIR)/mm/pmm.o: src/kernel/mm/pmm.h
$(OBJDIR)/drv/fb.o: $(FONT_H) src/kernel/drv/font_unicode.h
$(OBJDIR)/drv/font_unicode.o: src/kernel/drv/font_unicode.h $(UNICODE_FONT_H)

$(FONT_H): tools/gen_kernel_font.ps1
	powershell -NoProfile -ExecutionPolicy Bypass -File tools/gen_kernel_font.ps1 -Out $(FONT_H)

$(UNICODE_FONT_H): tools/gen_unicode_font.ps1
	powershell -NoProfile -ExecutionPolicy Bypass -File tools/gen_unicode_font.ps1 -Out $(UNICODE_FONT_H)

$(OBJDIR)/%.o-asm: src/kernel/%.asm | $(OBJDIR)
	powershell -NoProfile -Command "New-Item -ItemType Directory -Force (Split-Path '$@' -Parent) | Out-Null"
	$(NASM) -f elf32 $< -o $@

$(OBJDIR)/kernel.elf: $(KERNEL_OBJS) linker.ld | $(OBJDIR)
	$(LD) $(LDFLAGS) -o $@ $(KERNEL_OBJS)

$(IMAGE): $(OBJDIR)/kernel.elf tools/mkbootimg.py
	$(PYTHON) tools/mkbootimg.py \
		--kernel $(OBJDIR)/kernel.elf \
		--out $(IMAGE) \
		--limine-dir "$(LIMINE_DIR)" \
		--boot-partition-start $(BOOT_PARTITION_START) \
		--boot-partition-sectors $(BOOT_PARTITION_SECTORS) \
		--fs-start $(FS_START_SECTOR) \
		--fs-sectors $(FS_SECTORS)

# GCC-compiled user program → initrd
$(BUILD)/user:
	powershell -NoProfile -Command "New-Item -ItemType Directory -Force '$(BUILD)/user' | Out-Null"

$(BUILD)/user/user.ld: Makefile | $(BUILD)/user
	@echo 'ENTRY(_start)' > $@
	@echo 'SECTIONS { . = 0x20000000; .text : { *(.text.entry) *(.text*) } .rodata : { *(.rodata*) } .data : { *(.data*) } .bss : { *(.bss*) *(COMMON) } }' >> $@

$(BUILD)/user/crt0.o: src/user/libc/crt0.c src/user/libc/libc.h | $(BUILD)/user
	$(CC) $(UCFLAGS) -c src/user/libc/crt0.c -o $(BUILD)/user/crt0.o

$(BUILD)/user/libc.o: src/user/libc/libc.c src/user/libc/libc.h | $(BUILD)/user
	$(CC) $(UCFLAGS) -c src/user/libc/libc.c -o $(BUILD)/user/libc.o

$(BUILD)/user/guiapp.o: src/user/libc/guiapp.c src/user/libc/guiapp.h src/user/libc/libc.h | $(BUILD)/user
	$(CC) $(UCFLAGS) -c src/user/libc/guiapp.c -o $(BUILD)/user/guiapp.o

$(BUILD)/user/hello.o: src/user/bin/hello.c src/user/libc/libc.h | $(BUILD)/user
	$(CC) $(UCFLAGS) -c src/user/bin/hello.c -o $(BUILD)/user/hello.o

$(BUILD)/user/shell.o: src/user/bin/shell.c src/user/libc/libc.h | $(BUILD)/user
	$(CC) $(UCFLAGS) -c src/user/bin/shell.c -o $(BUILD)/user/shell.o

$(BUILD)/user/nano.o: src/user/bin/nano.c src/user/libc/libc.h | $(BUILD)/user
	$(CC) $(UCFLAGS) -c src/user/bin/nano.c -o $(BUILD)/user/nano.o

$(BUILD)/user/basm.o: src/user/bin/basm.c src/user/libc/libc.h | $(BUILD)/user
	$(CC) $(UCFLAGS) -c src/user/bin/basm.c -o $(BUILD)/user/basm.o

$(BUILD)/user/basm_engine.o: src/user/bin/basm.c src/user/bin/basm.h src/user/libc/libc.h | $(BUILD)/user
	$(CC) $(UCFLAGS) -DBASM_LIBRARY -Isrc/user/bin -c src/user/bin/basm.c -o $@

$(BUILD)/user/bcc.o: src/user/bin/bcc.c src/user/bin/basm.h src/user/libc/libc.h | $(BUILD)/user
	$(CC) $(UCFLAGS) -Isrc/user/bin -c src/user/bin/bcc.c -o $@

$(BUILD)/user/gui.o: src/user/bin/gui.c src/user/bin/pinyin_data.h $(USER_HEADERS) | $(BUILD)/user
	$(CC) $(UCFLAGS) -c src/user/bin/gui.c -o $(BUILD)/user/gui.o

$(LODEPNG_OBJ): $(LODEPNG_DIR)/lodepng.c $(LODEPNG_DIR)/lodepng.h src/user/libc/libc.h | $(BUILD)/user
	$(CC) $(UCFLAGS) $(LODEPNG_FLAGS) -c $(LODEPNG_DIR)/lodepng.c -o $@

$(BUILD)/user/browser.o: src/user/bin/browser.c $(USER_HEADERS) $(LODEPNG_DIR)/lodepng.h | $(BUILD)/user
	$(CC) $(UCFLAGS) $(LODEPNG_FLAGS) -c src/user/bin/browser.c -o $@

$(NETSURF_PORT_OBJ): src/user/ports/netsurf/buzzos_platform.c \
		src/user/ports/netsurf/buzzos_platform.h src/user/libc/libc.h | $(BUILD)/user
	$(CC) $(UCFLAGS) -Isrc/user/ports/netsurf -c $< -o $@

$(BUILD)/user/nsporttest.o: src/user/bin/nsporttest.c $(USER_HEADERS) \
		src/user/ports/netsurf/buzzos_platform.h | $(BUILD)/user
	$(CC) $(UCFLAGS) -Isrc/user/ports/netsurf -c $< -o $@

$(BUILD)/user/%.o: src/user/bin/%.c $(USER_HEADERS) | $(BUILD)/user
	$(CC) $(UCFLAGS) -c $< -o $@

$(USER_ELF): $(BUILD)/user/crt0.o $(BUILD)/user/libc.o $(BUILD)/user/guiapp.o $(BUILD)/user/hello.o $(BUILD)/user/user.ld | $(BUILD)/user
	$(LD) -m elf_i386 -T $(BUILD)/user/user.ld -nostdlib -o $@ \
		$(BUILD)/user/crt0.o $(BUILD)/user/libc.o $(BUILD)/user/guiapp.o $(BUILD)/user/hello.o
	$(OBJCOPY) --strip-sections $@

$(SHELL_ELF): $(BUILD)/user/crt0.o $(BUILD)/user/libc.o $(BUILD)/user/guiapp.o $(BUILD)/user/shell.o $(BUILD)/user/user.ld | $(BUILD)/user
	$(LD) -m elf_i386 -T $(BUILD)/user/user.ld -nostdlib -o $@ \
		$(BUILD)/user/crt0.o $(BUILD)/user/libc.o $(BUILD)/user/guiapp.o $(BUILD)/user/shell.o
	$(OBJCOPY) --strip-sections $@

$(NANO_ELF): $(BUILD)/user/crt0.o $(BUILD)/user/libc.o $(BUILD)/user/guiapp.o $(BUILD)/user/nano.o $(BUILD)/user/user.ld | $(BUILD)/user
	$(LD) -m elf_i386 -T $(BUILD)/user/user.ld -nostdlib -o $@ \
		$(BUILD)/user/crt0.o $(BUILD)/user/libc.o $(BUILD)/user/guiapp.o $(BUILD)/user/nano.o
	$(OBJCOPY) --strip-sections $@

$(BASM_ELF): $(BUILD)/user/crt0.o $(BUILD)/user/libc.o $(BUILD)/user/guiapp.o $(BUILD)/user/basm.o $(BUILD)/user/user.ld | $(BUILD)/user
	$(LD) -m elf_i386 -T $(BUILD)/user/user.ld -nostdlib -o $@ \
		$(BUILD)/user/crt0.o $(BUILD)/user/libc.o $(BUILD)/user/guiapp.o $(BUILD)/user/basm.o
	$(OBJCOPY) --strip-sections $@

$(BCC_ELF): $(BUILD)/user/crt0.o $(BUILD)/user/libc.o $(BUILD)/user/guiapp.o $(BUILD)/user/bcc.o $(BUILD)/user/basm_engine.o $(BUILD)/user/user.ld | $(BUILD)/user
	$(LD) -m elf_i386 -T $(BUILD)/user/user.ld -nostdlib -o $@ \
		$(BUILD)/user/crt0.o $(BUILD)/user/libc.o $(BUILD)/user/guiapp.o \
		$(BUILD)/user/bcc.o $(BUILD)/user/basm_engine.o
	$(OBJCOPY) --strip-sections $@

$(GUI_ELF): $(BUILD)/user/crt0.o $(BUILD)/user/libc.o $(BUILD)/user/guiapp.o $(BUILD)/user/gui.o $(BUILD)/user/user.ld | $(BUILD)/user
	$(LD) -m elf_i386 -T $(BUILD)/user/user.ld -nostdlib -o $@ \
		$(BUILD)/user/crt0.o $(BUILD)/user/libc.o $(BUILD)/user/guiapp.o $(BUILD)/user/gui.o
	$(OBJCOPY) --strip-sections $@

$(BUILD)/user/browser.elf: $(BUILD)/user/crt0.o $(BUILD)/user/libc.o $(BUILD)/user/guiapp.o \
		$(BUILD)/user/browser.o $(LODEPNG_OBJ) $(BUILD)/user/user.ld | $(BUILD)/user
	$(LD) -m elf_i386 -T $(BUILD)/user/user.ld -nostdlib -o $@ \
		$(BUILD)/user/crt0.o $(BUILD)/user/libc.o $(BUILD)/user/guiapp.o \
		$(BUILD)/user/browser.o $(LODEPNG_OBJ)
	$(OBJCOPY) --strip-sections $@

$(BUILD)/user/doom.elf: $(BUILD)/user/crt0.o $(BUILD)/user/libc.o $(BUILD)/user/guiapp.o \
		$(DOOM_SRCS) $(BUILD)/user/user.ld | $(BUILD)/user
	powershell -NoProfile -ExecutionPolicy Bypass -File tools/build-doom.ps1 -Output $@

$(NSPORTTEST_ELF): $(BUILD)/user/crt0.o $(BUILD)/user/libc.o \
		$(BUILD)/user/nsporttest.o $(NETSURF_PORT_OBJ) $(BUILD)/user/user.ld | $(BUILD)/user
	$(LD) -m elf_i386 -T $(BUILD)/user/user.ld -nostdlib -o $@ \
		$(BUILD)/user/crt0.o $(BUILD)/user/libc.o \
		$(BUILD)/user/nsporttest.o $(NETSURF_PORT_OBJ)
	$(OBJCOPY) --strip-sections $@

$(NSHTMLTEST_ELF): $(BUILD)/user/crt0.o $(BUILD)/user/libc.o \
		src/user/bin/nshtmltest.c tools/build-netsurf-core.ps1 \
		tools/gen-netsurf-aliases.py tools/gen-netsurf-entities.py \
		tools/gen-netsurf-elements.py $(BUILD)/user/user.ld | $(BUILD)/user
	powershell -NoProfile -ExecutionPolicy Bypass -File tools/build-netsurf-core.ps1

$(NETSURF_ELF): $(NSHTMLTEST_ELF) $(BUILD)/user/crt0.o $(BUILD)/user/libc.o \
		$(BUILD)/user/guiapp.o \
		tools/build-netsurf-engine.ps1 tools/fetch-netsurf.ps1 \
		tools/embed-netsurf-resources.py \
		third_party/netsurf-reference/resources/default.css \
		third_party/netsurf-reference/resources/adblock.css \
		third_party/netsurf-reference/resources/internal.css \
		third_party/netsurf-reference/resources/quirks.css \
		src/user/ports/netsurf/buzzos_build_config.h \
		src/user/ports/netsurf/buzzos_gui.c \
		src/user/ports/netsurf/buzzos_http_fetch.c \
		src/user/ports/netsurf/buzzos_http_fetch.h \
		src/user/ports/netsurf/buzzos_tls.c \
		src/user/ports/netsurf/buzzos_tls.h \
		src/user/ports/netsurf/buzzos_ca_bundle.inc \
		src/user/third_party/bearssl/inc/bearssl.h \
		src/user/third_party/bearssl/src/ssl/ssl_engine.c \
		src/user/ports/netsurf/buzzos_png.c \
		src/user/ports/netsurf/buzzos_png.h \
		src/user/third_party/lodepng/lodepng.c \
		src/user/third_party/lodepng/lodepng.h \
		$(FONT_H) \
		src/user/ports/netsurf/buzzos_gui_plot.c \
		src/user/ports/netsurf/buzzos_gui_plot.h $(BUILD)/user/user.ld | $(BUILD)/user
	powershell -NoProfile -ExecutionPolicy Bypass -File tools/build-netsurf-engine.ps1 -Link

$(NSMONKEY_ELF): $(NETSURF_ELF)

$(BUILD)/user/%.elf: $(BUILD)/user/crt0.o $(BUILD)/user/libc.o $(BUILD)/user/guiapp.o $(BUILD)/user/%.o $(BUILD)/user/user.ld | $(BUILD)/user
	$(LD) -m elf_i386 -T $(BUILD)/user/user.ld -nostdlib -o $@ \
		$(BUILD)/user/crt0.o $(BUILD)/user/libc.o $(BUILD)/user/guiapp.o $(BUILD)/user/$*.o
	$(OBJCOPY) --strip-sections $@

$(DEMO_WAV): tools/gen_demo_wav.py
	powershell -NoProfile -Command "New-Item -ItemType Directory -Force '$(BUILD)/assets' | Out-Null"
	$(PYTHON) tools/gen_demo_wav.py --out $@

$(INITRD_H): $(USER_ELFS) $(DEMO_WAV) tools/mkinitrd.py
	powershell -NoProfile -Command "New-Item -ItemType Directory -Force '$(GENERATED_DIR)' | Out-Null"
	$(PYTHON) tools/mkinitrd.py /hello $(USER_ELF) /bin/sh $(SHELL_ELF) \
		/bin/nano $(NANO_ELF) /bin/basm $(BASM_ELF) /bin/gui $(GUI_ELF) \
		/bin/bcc $(BCC_ELF) \
		/bin/futexhold $(FUTEXHOLD_ELF) /bin/cat $(CAT_ELF) /bin/echo $(ECHO_ELF) \
		/bin/faulttest $(FAULTTEST_ELF) /bin/socketleak $(SOCKETLEAK_ELF) \
		/bin/heaptest $(HEAPTEST_ELF) /bin/audiotest $(AUDIOTEST_ELF) \
		/bin/nsporttest $(NSPORTTEST_ELF) \
		/bin/nshtmltest $(NSHTMLTEST_ELF) \
		/bin/netsurf $(NETSURF_ELF) \
		/bin/browser $(NETSURF_ELF) \
		/share/buzzos-demo.wav $(DEMO_WAV) \
		$(foreach app,$(filter-out browser,$(GUI_APP_NAMES)),/bin/$(app) $(BUILD)/user/$(app).elf) > $@

$(APP_REGISTRY_H): tools/gen_app_registry.py Makefile $(GUI_APP_META) $(BASM_EXAMPLE) $(BCC_EXAMPLE) $(UTF8_EXAMPLE)
	powershell -NoProfile -Command "New-Item -ItemType Directory -Force '$(GENERATED_DIR)' | Out-Null"
	$(PYTHON) tools/gen_app_registry.py --apps "$(GUI_APP_NAMES)" \
		--seed "/fs/basm-full.asm=$(BASM_EXAMPLE)" \
		--seed "/fs/hello.c=$(BCC_EXAMPLE)" \
		--seed "/fs/utf8.txt=$(UTF8_EXAMPLE)" --out $@

.PHONY: user
user: $(INITRD_H)
	@echo "User program built and initrd updated. Run 'make' to rebuild kernel."

doctor:
	$(PYTHON) tools/doctor.py --python "$(PYTHON)" --make "$(MAKE)" --qemu "$(QEMU)"

run: $(IMAGE)
	$(QEMU) -cpu max -m 256 -drive format=raw,file=$(IMAGE) -serial stdio -no-reboot -vga std -audiodev dsound,id=audio0 -device AC97,audiodev=audio0 -netdev user,id=n0 -device ne2k_isa,netdev=n0,iobase=0x300,irq=10

run-current:
	powershell -NoProfile -Command "if (!(Test-Path '$(IMAGE)')) { throw '$(IMAGE) does not exist. Run make first.' }"
	$(QEMU) -cpu max -m 256 -drive format=raw,file=$(IMAGE) -serial stdio -no-reboot -vga std -audiodev dsound,id=audio0 -device AC97,audiodev=audio0 -netdev user,id=n0 -device ne2k_isa,netdev=n0,iobase=0x300,irq=10

run-local:
	powershell -NoProfile -ExecutionPolicy Bypass -File scripts/run-local.ps1 -Qemu "$(QEMU)"

run-gui:
	powershell -NoProfile -ExecutionPolicy Bypass -File scripts/run-local.ps1 -Qemu "$(QEMU)" -Command gui

check-project: $(IMAGE)
	$(PYTHON) tools/check_project.py

app-check: $(GUI_APP_ELFS) $(APP_REGISTRY_H)
	$(PYTHON) tools/check_project.py --apps-only

app-registry: $(APP_REGISTRY_H)
	@echo "Generated $(APP_REGISTRY_H)"

fs-check: $(IMAGE)
	$(PYTHON) tools/check_minifs.py --image "$(FS_IMAGE)"

fs-ls: $(IMAGE)
	$(PYTHON) tools/check_minifs.py --image "$(FS_IMAGE)" --list

fs-repair: $(IMAGE)
	$(PYTHON) tools/check_minifs.py --image "$(FS_IMAGE)" --repair --out "$(FS_REPAIR_IMAGE)"

smoke: $(IMAGE)
	powershell -NoProfile -ExecutionPolicy Bypass -File scripts/smoke.ps1 -Image $(IMAGE) -Qemu "$(QEMU)"

fs-check-smoke: smoke
	$(PYTHON) tools/check_minifs.py --image "$(FS_TEST_IMAGE)"

fs-check-negative: fs-check-smoke
	$(PYTHON) tools/check_minifs_negative.py --image "$(FS_TEST_IMAGE)"

fs-check-repair: fs-check-smoke
	$(PYTHON) tools/check_minifs_repair.py --image "$(FS_TEST_IMAGE)"

gui-smoke: $(IMAGE)
	powershell -NoProfile -ExecutionPolicy Bypass -File scripts/gui-smoke.ps1 -Image $(IMAGE) -Qemu "$(QEMU)" -PythonPath "$(PYTHON)"

report: $(IMAGE)
	$(PYTHON) tools/project_report.py --out "$(BUILD)/project-report.md" --print --python "$(PYTHON)" --make "$(MAKE)" --qemu "$(QEMU)"

verify: check-project smoke fs-check-smoke fs-check-negative fs-check-repair gui-smoke

image-reset-fs: $(OBJDIR)/kernel.elf tools/mkbootimg.py
	$(PYTHON) tools/mkbootimg.py \
		--kernel $(OBJDIR)/kernel.elf \
		--out $(IMAGE) \
		--limine-dir "$(LIMINE_DIR)" \
		--boot-partition-start $(BOOT_PARTITION_START) \
		--boot-partition-sectors $(BOOT_PARTITION_SECTORS) \
		--fs-start $(FS_START_SECTOR) \
		--fs-sectors $(FS_SECTORS) \
		--reset-fs

new-app:
	$(PYTHON) tools/new_app.py $(APP)


clean:
	powershell -NoProfile -Command "if (Test-Path -LiteralPath '$(BUILD)') { Remove-Item -LiteralPath '$(BUILD)' -Recurse -Force }"
