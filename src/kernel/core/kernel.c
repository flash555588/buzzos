#include <stddef.h>
#include <stdint.h>
#include "console.h"
#include "exec.h"
#include "apic.h"
#include "gdt.h"
#include "idt.h"
#include "keyboard.h"
#include "mouse.h"
#include "net.h"
#include "paging.h"
#include "fpu.h"
#include "pci.h"
#include "pmm.h"
#include "serial.h"
#include "audio.h"
#include "syscall.h"
#include "task.h"
#include "timer.h"
#include "vfs.h"
#include "fb.h"
#include "virtio_input.h"
#include "initrd.h"
#include "app_registry.h"

extern uint8_t __bss_start, __bss_end;

enum {
    MULTIBOOT2_BOOTLOADER_MAGIC = 0x36D76289u,
    MULTIBOOT2_TAG_END = 0,
    MULTIBOOT2_TAG_MMAP = 6,
    MULTIBOOT2_TAG_FRAMEBUFFER = 8,
};

struct multiboot2_tag {
    uint32_t type;
    uint32_t size;
};

struct multiboot2_tag_mmap {
    uint32_t type;
    uint32_t size;
    uint32_t entry_size;
    uint32_t entry_version;
};

struct multiboot2_mmap_entry {
    uint64_t addr;
    uint64_t len;
    uint32_t type;
    uint32_t zero;
} __attribute__((packed));

struct multiboot2_tag_framebuffer {
    uint32_t type;
    uint32_t size;
    uint64_t addr;
    uint32_t pitch;
    uint32_t width;
    uint32_t height;
    uint8_t bpp;
    uint8_t fb_type;
    uint16_t reserved;
} __attribute__((packed));

struct boot_framebuffer {
    uint64_t addr;
    uint32_t pitch;
    uint32_t width;
    uint32_t height;
    uint32_t bpp;
    int present;
};

static struct boot_framebuffer boot_fb;

static void zero_bss(void) {
    for (uint8_t *p = &__bss_start; p < &__bss_end; p++) *p = 0;
}

static void halt_forever(void) {
    for (;;)
        __asm__ volatile("hlt");
}

static void copy_multiboot_mmap(uint32_t info_addr) {
    struct multiboot2_tag *tag = (struct multiboot2_tag *)(uintptr_t)(info_addr + 8u);
    E820_COUNT = 0;
    while (tag->type != MULTIBOOT2_TAG_END) {
        if (tag->type == MULTIBOOT2_TAG_MMAP) {
            struct multiboot2_tag_mmap *mmap = (struct multiboot2_tag_mmap *)tag;
            uint8_t *entry_ptr = (uint8_t *)(uintptr_t)mmap + sizeof(*mmap);
            uint8_t *entry_end = (uint8_t *)(uintptr_t)tag + tag->size;
            while (entry_ptr + mmap->entry_size <= entry_end && E820_COUNT < 128) {
                struct multiboot2_mmap_entry *src =
                    (struct multiboot2_mmap_entry *)(uintptr_t)entry_ptr;
                struct e820_entry *dst = &E820_BUF[E820_COUNT++];
                dst->base = src->addr;
                dst->length = src->len;
                dst->type = src->type;
                dst->acpi_attrs = src->zero;
                entry_ptr += mmap->entry_size;
            }
            return;
        }
        tag = (struct multiboot2_tag *)(((uintptr_t)tag + tag->size + 7u) &
                                       ~(uintptr_t)7u);
    }
}

static void copy_multiboot_framebuffer(uint32_t info_addr) {
    struct multiboot2_tag *tag = (struct multiboot2_tag *)(uintptr_t)(info_addr + 8u);
    boot_fb.present = 0;
    while (tag->type != MULTIBOOT2_TAG_END) {
        if (tag->type == MULTIBOOT2_TAG_FRAMEBUFFER) {
            struct multiboot2_tag_framebuffer *fb =
                (struct multiboot2_tag_framebuffer *)tag;
            if (fb->addr && fb->width && fb->height && fb->pitch &&
                (fb->bpp == 16 || fb->bpp == 24 || fb->bpp == 32)) {
                boot_fb.addr = fb->addr;
                boot_fb.pitch = fb->pitch;
                boot_fb.width = fb->width;
                boot_fb.height = fb->height;
                boot_fb.bpp = fb->bpp;
                boot_fb.present = 1;
                serial_puts("[boot] framebuffer addr=");
                serial_puthex((uint32_t)fb->addr);
                serial_puts(" w=");
                serial_puthex(fb->width);
                serial_puts(" h=");
                serial_puthex(fb->height);
                serial_puts(" pitch=");
                serial_puthex(fb->pitch);
                serial_puts(" bpp=");
                serial_puthex(fb->bpp);
                serial_puts("\n");
            }
            return;
        }
        tag = (struct multiboot2_tag *)(((uintptr_t)tag + tag->size + 7u) &
                                       ~(uintptr_t)7u);
    }
    serial_puts("[boot] no multiboot2 framebuffer tag\n");
}

static void seed_file_if_missing(const char *path, const void *data, size_t size) {
    struct stat st;
    if (vfs_stat(path, &st) == 0)
        return;
    int written = vfs_write_file(path, data, size);
    if (written != (int)size) {
        serial_puts("[boot] failed to seed ");
        serial_puts(path);
        serial_puts("\n");
    }
}

static void seed_file_if_missing_or_size_changed(const char *path,
                                                 const void *data,
                                                 size_t size) {
    struct stat st;
    if (vfs_stat(path, &st) == 0 && st.st_size == size) {
        int fd = vfs_open(path);
        if (fd >= 0) {
            uint8_t buffer[512];
            const uint8_t *expected = (const uint8_t *)data;
            size_t offset = 0;
            int equal = 1;
            while (offset < size) {
                size_t want = size - offset;
                if (want > sizeof(buffer)) want = sizeof(buffer);
                int got = vfs_read(fd, buffer, want);
                if (got != (int)want) { equal = 0; break; }
                for (size_t i = 0; i < want; i++) {
                    if (buffer[i] != expected[offset + i]) {
                        equal = 0;
                        break;
                    }
                }
                if (!equal) break;
                offset += want;
            }
            vfs_close(fd);
            if (equal)
                return;
        }
    }
    int written = vfs_write_file(path, data, size);
    if (written != (int)size) {
        serial_puts("[boot] failed to refresh ");
        serial_puts(path);
        serial_puts("\n");
    }
}

static void seed_user_apps(void) {
    (void)vfs_mkdir("/fs/apps");
    for (size_t i = 0; i < app_seed_entry_count; i++) {
        const struct app_seed_entry *entry = &app_seed_entries[i];
        if (entry->refresh)
            seed_file_if_missing_or_size_changed(entry->path, entry->data, entry->size);
        else
            seed_file_if_missing(entry->path, entry->data, entry->size);
    }
}

__attribute__((noreturn))
void kernel_main(uint32_t mb_magic, uint32_t mb_info_addr) {
    zero_bss();
    serial_init();
    serial_puts("[boot] BuzzOS starting\n");
    if (mb_magic != MULTIBOOT2_BOOTLOADER_MAGIC) {
        serial_puts("[boot] bad multiboot2 magic\n");
        halt_forever();
    }
    copy_multiboot_mmap(mb_info_addr);
    copy_multiboot_framebuffer(mb_info_addr);
    apic_discover(mb_info_addr);
    gdt_install();
    idt_install();

    if (fpu_init())
        serial_puts("[boot] x87 + SSE/SSE2 FPU context enabled\n");
    else {
        serial_puts("[boot] CPU lacks required FXSR/SSE/SSE2\n");
        halt_forever();
    }
    syscall_init();
    pmm_init();
    pci_init();
    if (boot_fb.present)
        paging_set_framebuffer((uintptr_t)boot_fb.addr, boot_fb.pitch * boot_fb.height);
    paging_init();
    if (apic_initialize() < 0)
        serial_puts("[apic] unavailable; PCI INTx uses legacy PIC\n");
    (void)audio_init();
    if (boot_fb.present)
        fb_set_framebuffer(boot_fb.addr, boot_fb.width, boot_fb.height,
                           boot_fb.pitch, boot_fb.bpp);
    fb_init();
    if (console_init() < 0)
        serial_puts("[boot] framebuffer console unavailable\n");
    console_set_color(0x0F, 0x00);
    serial_puts("[boot] framebuffer init ok\n");
    keyboard_init();
    mouse_init();
    serial_puts("[boot] input devices ok\n");
    vfs_init();
    serial_puts("[boot] vfs init ok\n");
    vfs_mkdir("/bin");
    vfs_mkdir("/share");
    ramfs_register("/hello", initrd_hello_data, INITRD_HELLO_SIZE);
    ramfs_register("/bin/sh", initrd_bin_sh_data, INITRD_BIN_SH_SIZE);
    ramfs_register("/bin/nano", initrd_bin_nano_data, INITRD_BIN_NANO_SIZE);
    ramfs_register("/bin/basm", initrd_bin_basm_data, INITRD_BIN_BASM_SIZE);
    ramfs_register("/bin/bcc", initrd_bin_bcc_data, INITRD_BIN_BCC_SIZE);
    ramfs_register("/bin/lua", initrd_bin_lua_data, INITRD_BIN_LUA_SIZE);
    ramfs_register("/bin/gui", initrd_bin_gui_data, INITRD_BIN_GUI_SIZE);
    ramfs_register("/bin/futexhold", initrd_bin_futexhold_data, INITRD_BIN_FUTEXHOLD_SIZE);
    ramfs_register("/bin/cat", initrd_bin_cat_data, INITRD_BIN_CAT_SIZE);
    ramfs_register("/bin/echo", initrd_bin_echo_data, INITRD_BIN_ECHO_SIZE);
    ramfs_register("/bin/faulttest", initrd_bin_faulttest_data, INITRD_BIN_FAULTTEST_SIZE);
    ramfs_register("/bin/socketleak", initrd_bin_socketleak_data, INITRD_BIN_SOCKETLEAK_SIZE);
    ramfs_register("/bin/netstress", initrd_bin_netstress_data, INITRD_BIN_NETSTRESS_SIZE);
    ramfs_register("/bin/heaptest", initrd_bin_heaptest_data, INITRD_BIN_HEAPTEST_SIZE);
    ramfs_register("/bin/audiotest", initrd_bin_audiotest_data, INITRD_BIN_AUDIOTEST_SIZE);
    ramfs_register("/bin/gputest", initrd_bin_gputest_data, INITRD_BIN_GPUTEST_SIZE);
    ramfs_register("/bin/nsporttest", initrd_bin_nsporttest_data, INITRD_BIN_NSPORTTEST_SIZE);
    ramfs_register("/bin/nshtmltest", initrd_bin_nshtmltest_data, INITRD_BIN_NSHTMLTEST_SIZE);
    ramfs_register("/bin/netsurf", initrd_bin_netsurf_data, INITRD_BIN_NETSURF_SIZE);
    ramfs_register("/share/buzzos-demo.wav", initrd_share_buzzos_demo_wav_data,
                   INITRD_SHARE_BUZZOS_DEMO_WAV_SIZE);
    ramfs_register("/share/buzzos-demo.mp3", initrd_share_buzzos_demo_mp3_data,
                   INITRD_SHARE_BUZZOS_DEMO_MP3_SIZE);
    serial_puts("[boot] initrd files registered\n");
    seed_user_apps();
    serial_puts("[boot] user apps seeded\n");

    sched_init();
    timer_init();
    /* Prefer a real absolute tablet for graphical hosts.  This is initialized
     * after the scheduler because its IRQ publishes GUI wakeups.  Machines
     * without VirtIO input keep the PS/2 mouse initialized above. */
    (void)virtio_input_init();
    (void)audio_start_worker();
    /* VMware's hosted NAT backend can take tens of milliseconds to attach.
     * Run DHCP only after the PIT and scheduler exist so receive waits use a
     * real deadline instead of a CPU-speed-dependent early-boot spin. */
    while (timer_ticks() == 0)
        __asm__ volatile("hlt");
    net_init();
    serial_puts("[boot] net init ok\n");

    if (exec_start(initrd_bin_sh_data, INITRD_BIN_SH_SIZE, "sh", 0) < 0)
        serial_puts("[boot] failed to start user shell\n");
    else
        serial_puts("[boot] user shell started\n");
    for (;;) { task_yield(); }
}
