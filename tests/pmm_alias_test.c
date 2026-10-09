#ifdef NDEBUG
#error "PMM regression requires enabled assertions"
#endif
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "../src/kernel/mm/pmm.h"
#include "../src/kernel/arch/x86_64/paging.h"

static struct e820_entry test_map[] = {
    {0, UINT64_C(0x40000000), E820_USABLE, 0},
};

#undef E820_BUF
#undef E820_COUNT
#define E820_BUF test_map
#define E820_COUNT 1

#include "../src/kernel/mm/pmm.c"

uint8_t __kernel_start;
uint8_t __kernel_end;

void task_yield(void) {
    assert(!"Unexpected allocator lock contention");
}

void serial_puts(const char *value) { (void)value; }
void serial_puthex(uint32_t value) { (void)value; }

static void check_address(uintptr_t address, size_t pages) {
    assert(address != 0);
    assert(address % PAGE_SIZE == 0);
    uintptr_t end = address + pages * PAGE_SIZE;
    assert(end <= KERNEL_FB_VIRT || address >= KERNEL_FB_VIRT + KERNEL_FB_SIZE);
    assert(end <= KERNEL_MMIO_VIRT || address >= KERNEL_MMIO_VIRT + KERNEL_MMIO_SIZE);
    assert(end <= test_map[0].length);
}

int main(void) {
    assert((uintptr_t)&__kernel_start >= PMM_MANAGED_LIMIT);
    pmm_init();
    size_t expected = (size_t)(test_map[0].length / PAGE_SIZE) - 1 -
        (size_t)((KERNEL_FB_SIZE + KERNEL_MMIO_SIZE) / PAGE_SIZE);
    assert(pmm_free_pages_snapshot() == expected);

    alloc_rover = (KERNEL_FB_VIRT - PAGE_SIZE) / PAGE_SIZE;
    uintptr_t crossing = pmm_alloc_pages(2);
    check_address(crossing, 2);
    assert(crossing == KERNEL_MMIO_VIRT + KERNEL_MMIO_SIZE);
    pmm_free_pages(crossing, 2);
    assert(pmm_free_pages_snapshot() == expected);

    size_t allocated = 0;
    uintptr_t last = 0;
    for (;;) {
        uintptr_t address = pmm_alloc_pages(1);
        if (!address)
            break;
        check_address(address, 1);
        last = address;
        allocated++;
    }
    assert(allocated == expected);
    assert(pmm_free_pages_snapshot() == 0);
    pmm_free_pages(last, 1);
    assert(pmm_alloc_pages(1) == last);
    assert(pmm_alloc_pages(1) == 0);

    test_map[0].length = UINT64_C(0x08000000);
    pmm_init();
    assert(pmm_free_pages_snapshot() == test_map[0].length / PAGE_SIZE - 1);
    size_t initial = pmm_free_pages_snapshot();
    size_t user_count = 0;
    uintptr_t user_last = 0;
    while ((last = pmm_alloc_user_pages(1)) != 0) {
        user_last = last;
        user_count++;
    }
    assert(user_count == initial - PMM_KERNEL_RESERVE_PAGES);
    assert(pmm_free_pages_snapshot() == PMM_KERNEL_RESERVE_PAGES);
    assert(!pmm_alloc_user_pages(1));
    assert(!pmm_alloc_user_pages(SIZE_MAX));
    /* Existing processes remain alive; kernel allocation can still proceed. */
    uintptr_t emergency = pmm_alloc_pages(1);
    assert(emergency && pmm_free_pages_snapshot() == PMM_KERNEL_RESERVE_PAGES - 1);
    pmm_free_pages(user_last, 1);
    assert(!pmm_alloc_user_pages(1));
    pmm_free_pages(emergency, 1);
    assert(pmm_alloc_user_pages(1) == user_last);
    assert(pmm_free_pages_snapshot() == PMM_KERNEL_RESERVE_PAGES);
    puts("PASS real PMM: user admission preserves kernel headroom, reuse and no overflow");
    puts("PASS real PMM: alias windows excluded, contiguous boundary, exhaustion/reuse, low RAM");
    return 0;
}
