#include <stddef.h>
#include <stdint.h>
#include "exec.h"
#include "gdt.h"
#include "idt.h"
#include "keyboard.h"
#include "mouse.h"
#include "net.h"
#include "paging.h"
#include "pmm.h"
#include "serial.h"
#include "syscall.h"
#include "task.h"
#include "timer.h"
#include "vfs.h"
#include "fb.h"
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
        tag = (struct multiboot2_tag *)(uintptr_t)(((uint32_t)(uintptr_t)tag + tag->size + 7u) & ~7u);
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
                (fb->bpp == 8 || fb->bpp == 16 || fb->bpp == 24 || fb->bpp == 32)) {
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
        tag = (struct multiboot2_tag *)(uintptr_t)(((uint32_t)(uintptr_t)tag + tag->size + 7u) & ~7u);
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
    if (vfs_stat(path, &st) == 0 && st.st_size == size)
        return;
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
    gdt_install();
    idt_install();

    /* Enable x87 FPU: clear CR0.EM (bit 2), set CR0.MP (bit 1),
     * then finit to reset the FPU state. */
    {
        uint32_t cr0;
        __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
        cr0 = (cr0 & ~(1u << 2)) | (1u << 1);  /* clear EM, set MP */
        __asm__ volatile("mov %0, %%cr0" : : "r"(cr0));
        __asm__ volatile("finit");
        serial_puts("[boot] x87 FPU enabled\n");
    }
    syscall_init();
    pmm_init();
    if (boot_fb.present)
        paging_set_framebuffer((uintptr_t)boot_fb.addr, boot_fb.pitch * boot_fb.height);
    paging_init();
    if (boot_fb.present)
        fb_set_framebuffer(boot_fb.addr, boot_fb.width, boot_fb.height,
                           boot_fb.pitch, boot_fb.bpp);
    fb_init();
    fb_set_color(0x0F, 0x00);
    serial_puts("[boot] framebuffer init ok\n");
    keyboard_init();
    mouse_init();
    serial_puts("[boot] input devices ok\n");
    vfs_init();
    serial_puts("[boot] vfs init ok\n");
    vfs_mkdir("/bin");
    ramfs_register("/hello", initrd_hello_data, INITRD_HELLO_SIZE);
    ramfs_register("/bin/sh", initrd_bin_sh_data, INITRD_BIN_SH_SIZE);
    ramfs_register("/bin/nano", initrd_bin_nano_data, INITRD_BIN_NANO_SIZE);
    ramfs_register("/bin/basm", initrd_bin_basm_data, INITRD_BIN_BASM_SIZE);
    ramfs_register("/bin/bcc", initrd_bin_bcc_data, INITRD_BIN_BCC_SIZE);
    ramfs_register("/bin/gui", initrd_bin_gui_data, INITRD_BIN_GUI_SIZE);
    ramfs_register("/bin/futexhold", initrd_bin_futexhold_data, INITRD_BIN_FUTEXHOLD_SIZE);
    ramfs_register("/bin/cat", initrd_bin_cat_data, INITRD_BIN_CAT_SIZE);
    ramfs_register("/bin/echo", initrd_bin_echo_data, INITRD_BIN_ECHO_SIZE);
    ramfs_register("/bin/faulttest", initrd_bin_faulttest_data, INITRD_BIN_FAULTTEST_SIZE);
    ramfs_register("/bin/socketleak", initrd_bin_socketleak_data, INITRD_BIN_SOCKETLEAK_SIZE);
    serial_puts("[boot] initrd files registered\n");
    seed_user_apps();
    serial_puts("[boot] user apps seeded\n");
    net_init();
    serial_puts("[boot] net init ok\n");

    sched_init();
    timer_init();
    if (exec_start(initrd_bin_sh_data, INITRD_BIN_SH_SIZE, "sh", 0) < 0)
        serial_puts("[boot] failed to start user shell\n");
    else
        serial_puts("[boot] user shell started\n");
    for (;;) { task_yield(); }
}
