#include "pmm.h"
#include "serial.h"

/* ------------------------------------------------------------------ */
/*  Bitmap (1 bit per 4 KiB page)                                      */
/* ------------------------------------------------------------------ */

#define MAX_PAGES          65536      /* bitmap capacity: 256 MiB */
#define PMM_MANAGED_LIMIT  0x10000000u /* current paging maps low 256 MiB */
#define PMM_MANAGED_PAGES  (PMM_MANAGED_LIMIT / PAGE_SIZE)
#define BITMAP_WORDS       (MAX_PAGES / 32)
#define PAGE_MASK          ((uint64_t)PAGE_SIZE - 1u)

static uint32_t bitmap[BITMAP_WORDS];
static size_t   managed_pages;
static size_t   free_pages;

static void bit_set(size_t idx)   { bitmap[idx / 32] |=  (1u << (idx % 32)); }
static void bit_clr(size_t idx)   { bitmap[idx / 32] &= ~(1u << (idx % 32)); }
static int  bit_get(size_t idx)   { return (bitmap[idx / 32] >> (idx % 32)) & 1; }

/* ------------------------------------------------------------------ */
/*  Mark a physical range as used or free                              */
/* ------------------------------------------------------------------ */

static uint64_t page_align_down(uint64_t value) {
    return value & ~PAGE_MASK;
}

static uint64_t page_align_up(uint64_t value) {
    if (value > UINT64_MAX - PAGE_MASK)
        return UINT64_MAX & ~PAGE_MASK;
    return (value + PAGE_MASK) & ~PAGE_MASK;
}

static size_t mark_range(uintptr_t base, size_t bytes, int used) {
    if (bytes == 0)
        return 0;

    uint64_t start = (uint64_t)base;
    uint64_t end = start + (uint64_t)bytes;
    if (end < start)
        end = UINT64_MAX;

    if (used) {
        start = page_align_down(start);
        end = page_align_up(end);
    } else {
        start = page_align_up(start);
        end = page_align_down(end);
    }

    if (end <= start || start >= PMM_MANAGED_LIMIT)
        return 0;
    if (end > PMM_MANAGED_LIMIT)
        end = PMM_MANAGED_LIMIT;

    size_t changed = 0;
    size_t first_page = (size_t)(start / PAGE_SIZE);
    size_t last_page = (size_t)(end / PAGE_SIZE);
    for (size_t page = first_page; page < last_page && page < PMM_MANAGED_PAGES; page++) {
        int was_used = bit_get(page);
        if (used) {
            if (!was_used)
                changed++;
            bit_set(page);
        } else {
            if (was_used)
                changed++;
            bit_clr(page);
        }
    }
    return changed;
}

static size_t count_free_pages(void) {
    size_t count = 0;
    for (size_t i = 0; i < PMM_MANAGED_PAGES; i++) {
        if (!bit_get(i))
            count++;
    }
    return count;
}

/* ------------------------------------------------------------------ */
/*  Initialise                                                         */
/* ------------------------------------------------------------------ */

extern uint8_t __kernel_start, __kernel_end; /* image plus bootstrap stack */

void pmm_init(void) {
    serial_puts("[pmm] scanning E820 map...\n");

    /* Default: all pages used */
    for (size_t i = 0; i < BITMAP_WORDS; i++) bitmap[i] = ~0u;

    uint16_t entries = E820_COUNT;
    managed_pages = 0;
    free_pages = 0;

    serial_puts("[pmm] entries: ");
    serial_puthex(entries);
    serial_puts("\n");

    /* Mark usable regions as free */
    for (uint16_t i = 0; i < entries && i < 128; i++) {
        if (E820_BUF[i].type == E820_USABLE) {
            uint64_t base  = E820_BUF[i].base;
            uint64_t len   = E820_BUF[i].length;
            /* Only free memory the current kernel page tables can access. */
            if (base >= PMM_MANAGED_LIMIT) continue;
            if (len > (uint64_t)PMM_MANAGED_LIMIT - base)
                len = (uint64_t)PMM_MANAGED_LIMIT - base;
            managed_pages += mark_range((uintptr_t)base, (size_t)len, 0);
        }
    }

    /* Reserve the complete kernel allocation, including .bss and the
     * linker-placed bootstrap/idle stack that follows it. */
    uintptr_t kernel_start = (uintptr_t)&__kernel_start;
    uintptr_t kernel_end   = (uintptr_t)&__kernel_end;
    mark_range(kernel_start, kernel_end - kernel_start, 1);

    /* Reserve first 4 KiB (IVT / BDA) */
    mark_range(0, 0x1000, 1);

    free_pages = count_free_pages();

    serial_puts("[pmm] managed limit: ");
    serial_puthex(PMM_MANAGED_LIMIT);
    serial_puts(" bytes\n");
    serial_puts("[pmm] managed usable pages: ");
    serial_puthex((uint32_t)managed_pages);
    serial_puts("\n");
    serial_puts("[pmm] allocatable pages: ");
    serial_puthex((uint32_t)free_pages);
    serial_puts("\n");
}

/* ------------------------------------------------------------------ */
/*  Allocate `n` contiguous pages                                      */
/* ------------------------------------------------------------------ */

uintptr_t pmm_alloc_pages(size_t n) {
    if (n == 0 || n > PMM_MANAGED_PAGES || n > free_pages) return 0;

    for (size_t start = 0; start <= PMM_MANAGED_PAGES - n; start++) {
        int ok = 1;
        for (size_t j = 0; j < n; j++) {
            if (bit_get(start + j)) { ok = 0; break; }
        }
        if (ok) {
            for (size_t j = 0; j < n; j++) bit_set(start + j);
            free_pages -= n;
            return (uintptr_t)(start * PAGE_SIZE);
        }
    }
    return 0;  /* no contiguous block found */
}

/* ------------------------------------------------------------------ */
/*  Free                                                               */
/* ------------------------------------------------------------------ */

void pmm_free_pages(uintptr_t addr, size_t n) {
    if (n == 0 || n > PMM_MANAGED_PAGES)
        return;
    free_pages += mark_range(addr, n * PAGE_SIZE, 0);
}

void pmm_info(struct pmm_info *out) {
    if (!out)
        return;
    out->page_size = PAGE_SIZE;
    out->managed_limit = PMM_MANAGED_LIMIT;
    out->managed_pages = (uint32_t)managed_pages;
    out->free_pages = (uint32_t)free_pages;
    out->used_pages = (uint32_t)(managed_pages > free_pages ? managed_pages - free_pages : 0);
}

/* ------------------------------------------------------------------ */
/*  Debug dump                                                         */
/* ------------------------------------------------------------------ */

void pmm_dump(void) {
    serial_puts("\n--- E820 Memory Map ---\n");
    uint16_t n = E820_COUNT;
    for (uint16_t i = 0; i < n && i < 128; i++) {
        serial_puts("  base=0x");  serial_puthex((uint32_t)E820_BUF[i].base);
        serial_puts(" len=0x");   serial_puthex((uint32_t)E820_BUF[i].length);
        serial_puts(" type=");    serial_puthex(E820_BUF[i].type);
        serial_puts("\n");
    }
    serial_puts("--- end map ---\n");
}
