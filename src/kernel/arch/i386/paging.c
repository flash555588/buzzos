#include "paging.h"
#include "pmm.h"
#include "serial.h"
#include "user_bounds.h"

__attribute__((aligned(4096))) static uint32_t page_directory[1024];
__attribute__((aligned(4096))) static uint32_t page_table_low[KERNEL_LOW_TABLES][1024];
__attribute__((aligned(4096))) static uint32_t page_table_fb[KERNEL_FB_TABLES][1024];

static uintptr_t kernel_fb_phys = 0xE0000000u;
static uint32_t kernel_fb_size = KERNEL_FB_SIZE;

void paging_set_framebuffer(uintptr_t phys_addr, uint32_t size) {
    if (!phys_addr || !size)
        return;
    kernel_fb_phys = phys_addr & ~(uintptr_t)(PAGE_SIZE - 1u);
    kernel_fb_size = size + (uint32_t)(phys_addr - kernel_fb_phys);
    if (kernel_fb_size > KERNEL_FB_SIZE)
        kernel_fb_size = KERNEL_FB_SIZE;
}

static void zero_page(uint32_t *page) {
    for (int i = 0; i < 1024; i++)
        page[i] = 0;
}

static void flush_tlb(void) {
    __asm__ volatile("mov %%cr3, %%eax\nmov %%eax, %%cr3" ::: "eax", "memory");
}

static uint32_t user_pde_first(void) {
    return USER_SPACE_START >> 22;
}

static uint32_t user_pde_last(void) {
    return (USER_SPACE_END - 1u) >> 22;
}

static uint32_t pde_index(uint32_t va) {
    return va >> 22;
}

static uint32_t pte_index(uint32_t va) {
    return (va >> 12) & 0x3FFu;
}

static int add_overflows_u32(uint32_t a, uint32_t b, uint32_t *out) {
    if (b > 0xFFFFFFFFu - a)
        return 1;
    *out = a + b;
    return 0;
}

void paging_init(void) {
    serial_puts("[page] setting up paging...\n");

    for (int i = 0; i < 1024; i++) {
        page_directory[i] = 0;
        for (uint32_t t = 0; t < KERNEL_LOW_TABLES; t++)
            page_table_low[t][i] = 0;
        for (uint32_t t = 0; t < KERNEL_FB_TABLES; t++)
            page_table_fb[t][i] = 0;
    }

    /* Identity map low managed memory for kernel access. User processes get private
     * mappings for their low user window in their own page directory. */
    for (int i = 0; i < 1024; i++) {
        for (uint32_t t = 0; t < KERNEL_LOW_TABLES; t++) {
            uint32_t offset = (t * 1024u + (uint32_t)i) * PAGE_SIZE;
            page_table_low[t][i] = offset | PAGE_PRESENT | PAGE_RW;
        }
        for (uint32_t t = 0; t < KERNEL_FB_TABLES; t++) {
            uint32_t fb_offset = (t * 1024u + (uint32_t)i) * PAGE_SIZE;
            if (fb_offset < kernel_fb_size) {
                page_table_fb[t][i] = (uint32_t)(kernel_fb_phys + fb_offset) |
                                      PAGE_PRESENT | PAGE_RW | PAGE_WT | PAGE_CD;
            }
        }
    }
    for (uint32_t t = 0; t < KERNEL_LOW_TABLES; t++)
        page_directory[t] = ((uint32_t)(uintptr_t)page_table_low[t]) |
                            PAGE_PRESENT | PAGE_RW;
    for (uint32_t t = 0; t < KERNEL_FB_TABLES; t++)
        page_directory[(KERNEL_FB_VIRT >> 22) + t] =
            ((uint32_t)(uintptr_t)page_table_fb[t]) | PAGE_PRESENT | PAGE_RW;
    for (uint32_t t = 0; t < KERNEL_LOW_TABLES; t++)
        page_directory[768 + t] = ((uint32_t)(uintptr_t)page_table_low[t]) |
                                  PAGE_PRESENT | PAGE_RW;

    paging_switch((uint32_t)(uintptr_t)page_directory);
    uint32_t cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= 0x80010000;
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0));
    __asm__ volatile("jmp 1f; 1:");

    serial_puts("[page] paging enabled (user accessible)\n");
}

uint32_t paging_current_cr3(void) {
    uint32_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    return cr3;
}

uint32_t paging_kernel_cr3(void) {
    return (uint32_t)(uintptr_t)page_directory;
}

void paging_switch(uint32_t cr3) {
    __asm__ volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
}

uint32_t paging_create_user_space(void) {
    uint32_t *pd = (uint32_t *)(uintptr_t)pmm_alloc_pages(1);
    if (!pd) {
        return 0;
    }

    zero_page(pd);

    uint32_t first = user_pde_first();
    uint32_t last = user_pde_last();
    for (uint32_t pde = first; pde <= last; pde++) {
        uint32_t *user_pt = (uint32_t *)(uintptr_t)pmm_alloc_pages(1);
        if (!user_pt) {
            for (uint32_t prev = first; prev < pde; prev++) {
                if (pd[prev] & PAGE_PRESENT)
                    pmm_free_pages((uintptr_t)(pd[prev] & 0xFFFFF000u), 1);
            }
            pmm_free_pages((uintptr_t)pd, 1);
            return 0;
        }
        zero_page(user_pt);
        pd[pde] = ((uint32_t)(uintptr_t)user_pt) |
                  PAGE_PRESENT | PAGE_RW | PAGE_USER;
    }

    pd[0] = ((uint32_t)(uintptr_t)page_table_low[0]) |
            PAGE_PRESENT | PAGE_RW;
    for (uint32_t t = 1; t < KERNEL_LOW_TABLES; t++) {
        if (t >= first && t <= last)
            continue;
        pd[t] = ((uint32_t)(uintptr_t)page_table_low[t]) | PAGE_PRESENT | PAGE_RW;
    }
    for (uint32_t t = 0; t < KERNEL_FB_TABLES; t++)
        pd[(KERNEL_FB_VIRT >> 22) + t] =
            ((uint32_t)(uintptr_t)page_table_fb[t]) | PAGE_PRESENT | PAGE_RW;
    for (uint32_t t = 0; t < KERNEL_LOW_TABLES; t++)
        pd[768 + t] = ((uint32_t)(uintptr_t)page_table_low[t]) |
                      PAGE_PRESENT | PAGE_RW;

    flush_tlb();
    return (uint32_t)(uintptr_t)pd;
}

int paging_map_user_range(uint32_t va, uint32_t size) {
    uint32_t end;
    if (size == 0)
        return 0;
    if (add_overflows_u32(va, size, &end))
        return -1;
    if (va < USER_SPACE_START || end > USER_SPACE_END)
        return -1;

    uint32_t start = va & ~(PAGE_SIZE - 1u);
    end = (end + PAGE_SIZE - 1u) & ~(PAGE_SIZE - 1u);
    uint32_t *pd = (uint32_t *)(uintptr_t)paging_current_cr3();
    for (uint32_t cur = start; cur < end; cur += PAGE_SIZE) {
        uint32_t pde = pde_index(cur);
        if (!(pd[pde] & PAGE_PRESENT))
            return -1;
        uint32_t *user_pt = (uint32_t *)(uintptr_t)(pd[pde] & 0xFFFFF000u);
        uint32_t idx = pte_index(cur);
        if (user_pt[idx] & PAGE_PRESENT)
            continue;
        uintptr_t phys = pmm_alloc_pages(1);
        if (!phys)
            return -1;
        user_pt[idx] = (uint32_t)phys | PAGE_PRESENT | PAGE_RW | PAGE_USER;
    }
    flush_tlb();
    return 0;
}

int paging_user_range_accessible(uint32_t va, uint32_t size, int write) {
    if (size == 0)
        return 1;
    uint32_t last;
    if (va < USER_SPACE_START || add_overflows_u32(va, size - 1u, &last) ||
        last >= USER_SPACE_END)
        return 0;

    uint32_t *pd = (uint32_t *)(uintptr_t)paging_current_cr3();
    uint32_t cur = va & ~(PAGE_SIZE - 1u);
    uint32_t end_page = last & ~(PAGE_SIZE - 1u);
    for (;;) {
        uint32_t pde = pd[pde_index(cur)];
        if (!(pde & PAGE_PRESENT) || !(pde & PAGE_USER) ||
            (write && !(pde & PAGE_RW)))
            return 0;
        uint32_t *pt = (uint32_t *)(uintptr_t)(pde & 0xFFFFF000u);
        uint32_t pte = pt[pte_index(cur)];
        if (!(pte & PAGE_PRESENT) || !(pte & PAGE_USER) ||
            (write && !(pte & PAGE_RW)))
            return 0;
        if (cur == end_page)
            break;
        cur += PAGE_SIZE;
    }
    return 1;
}

int paging_set_user_range_writable(uint32_t va, uint32_t size, int writable) {
    if (size == 0)
        return 0;
    uint32_t last;
    if (va < USER_SPACE_START || add_overflows_u32(va, size - 1u, &last) ||
        last >= USER_SPACE_END)
        return -1;

    uint32_t *pd = (uint32_t *)(uintptr_t)paging_current_cr3();
    uint32_t cur = va & ~(PAGE_SIZE - 1u);
    uint32_t end_page = last & ~(PAGE_SIZE - 1u);
    for (;;) {
        uint32_t pde = pd[pde_index(cur)];
        if (!(pde & PAGE_PRESENT) || !(pde & PAGE_USER))
            return -1;
        uint32_t *pt = (uint32_t *)(uintptr_t)(pde & 0xFFFFF000u);
        uint32_t idx = pte_index(cur);
        if (!(pt[idx] & PAGE_PRESENT) || !(pt[idx] & PAGE_USER))
            return -1;
        if (writable)
            pt[idx] |= PAGE_RW;
        else
            pt[idx] &= ~PAGE_RW;
        if (cur == end_page)
            break;
        cur += PAGE_SIZE;
    }
    flush_tlb();
    return 0;
}

void paging_destroy_user_space(uint32_t cr3) {
    if (!cr3 || cr3 == paging_kernel_cr3())
        return;

    uint32_t *pd = (uint32_t *)(uintptr_t)cr3;
    uint32_t first = user_pde_first();
    uint32_t last = user_pde_last();
    for (uint32_t pde = first; pde <= last; pde++) {
        if (!(pd[pde] & PAGE_PRESENT))
            continue;
        uint32_t *user_pt = (uint32_t *)(uintptr_t)(pd[pde] & 0xFFFFF000u);
        for (uint32_t idx = 0; idx < 1024; idx++) {
            if (user_pt[idx] & PAGE_PRESENT) {
                pmm_free_pages((uintptr_t)(user_pt[idx] & 0xFFFFF000u), 1);
                user_pt[idx] = 0;
            }
        }
        pmm_free_pages((uintptr_t)user_pt, 1);
        pd[pde] = 0;
    }

    pmm_free_pages((uintptr_t)pd, 1);
}
