#ifndef BUZZOS_PMM_H
#define BUZZOS_PMM_H

#include <stddef.h>
#include <stdint.h>

struct e820_entry {
    uint64_t base;
    uint64_t length;
    uint32_t type;
    uint32_t acpi_attrs;
} __attribute__((packed));

/* Boot sector stores E820 entries at 0x500 and count at 0x4F8. */
#define E820_BUF    ((struct e820_entry *)0x500)
#define E820_COUNT  (*(uint16_t *)0x4F8)

enum { PAGE_SIZE = 4096, E820_USABLE = 1 };

struct pmm_info {
    uint32_t page_size;
    uint64_t managed_limit;
    uint32_t managed_pages;
    uint32_t free_pages;
    uint32_t used_pages;
};

void     pmm_init(void);
uintptr_t pmm_alloc_pages(size_t n);
void     pmm_free_pages(uintptr_t addr, size_t n);
void     pmm_info(struct pmm_info *out);
void     pmm_dump(void);

#endif
