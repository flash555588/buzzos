#include "paging.h"
#include "pmm.h"
#include "serial.h"
#include "task.h"
#include "user_bounds.h"

__attribute__((aligned(4096))) static uint32_t page_directory[1024];
__attribute__((aligned(4096))) static uint32_t page_table_low[KERNEL_LOW_TABLES][1024];
__attribute__((aligned(4096))) static uint32_t page_table_fb[KERNEL_FB_TABLES][1024];
__attribute__((aligned(4096))) static uint32_t page_table_mmio[1024];
__attribute__((aligned(4096))) static uint32_t page_table_apic[1024];

static uintptr_t kernel_fb_phys = 0xE0000000u;
static uint32_t kernel_fb_size = KERNEL_FB_SIZE;
static volatile int paging_locked;
static uint32_t mmio_next_page;

static void paging_lock(void) {
    while (__sync_lock_test_and_set(&paging_locked, 1))
        task_yield();
}

static void paging_unlock(void) {
    __sync_lock_release(&paging_locked);
}

uintptr_t paging_framebuffer_phys(void) {
    return kernel_fb_phys;
}

void paging_set_framebuffer(uintptr_t phys_addr, uint32_t size) {
    if (!phys_addr || !size)
        return;
    kernel_fb_phys = phys_addr & ~(uintptr_t)(PAGE_SIZE - 1u);
    /* Map the complete 16 MiB VGA aperture.  The boot mode may use only a
     * fraction of it, but Bochs VBE runtime mode switches can grow the
     * scanout without changing the linear-framebuffer base address. */
    kernel_fb_size = KERNEL_FB_SIZE;
}

static void zero_page(uint32_t *page) {
    for (int i = 0; i < 1024; i++)
        page[i] = 0;
}

static void flush_tlb(void) {
    __asm__ volatile("mov %%cr3, %%eax\nmov %%eax, %%cr3" ::: "eax", "memory");
}

static int configure_framebuffer_write_combining(void) {
    uint32_t eax = 1, ebx, ecx, edx;
    __asm__ volatile("cpuid"
                     : "+a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx));
    if (!(edx & (1u << 16)))
        return 0;
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0x277u));
    /* PWT|PCD selects PAT entry 3 for a 4 KiB PTE.  Reserve that entry for
     * write-combining; the kernel uses this combination only for framebuffer
     * pages. */
    lo = (lo & 0x00FFFFFFu) | (1u << 24);
    __asm__ volatile("wrmsr" : : "c"(0x277u), "a"(lo), "d"(hi));
    return 1;
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

static uint32_t *space_page_directory(uint32_t cr3) {
    return cr3 ? (uint32_t *)(uintptr_t)(cr3 & 0xFFFFF000u) : 0;
}

static uint32_t *ensure_user_page_table(uint32_t *pd, uint32_t pde) {
    if (pd[pde] & PAGE_PRESENT) {
        if (!(pd[pde] & PAGE_USER))
            return 0;
        return (uint32_t *)(uintptr_t)(pd[pde] & 0xFFFFF000u);
    }
    uintptr_t phys = pmm_alloc_pages(1);
    if (!phys)
        return 0;
    uint32_t *pt = (uint32_t *)(uintptr_t)phys;
    zero_page(pt);
    pd[pde] = (uint32_t)phys | PAGE_PRESENT | PAGE_RW | PAGE_USER;
    return pt;
}

static int add_overflows_u32(uint32_t a, uint32_t b, uint32_t *out) {
    if (b > 0xFFFFFFFFu - a)
        return 1;
    *out = a + b;
    return 0;
}

void paging_init(void) {
    serial_puts("[page] setting up paging...\n");
    int framebuffer_wc = configure_framebuffer_write_combining();

    for (int i = 0; i < 1024; i++) {
        page_directory[i] = 0;
        page_table_apic[i] =
            (KERNEL_APIC_MMIO_BASE + (uint32_t)i * PAGE_SIZE) |
            PAGE_PRESENT | PAGE_RW | PAGE_WT | PAGE_CD;
        page_table_mmio[i] = 0;
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
                                      PAGE_PRESENT | PAGE_RW |
                                      PAGE_WT | PAGE_CD;
            }
        }
    }
    for (uint32_t t = 0; t < KERNEL_LOW_TABLES; t++)
        page_directory[t] = ((uint32_t)(uintptr_t)page_table_low[t]) |
                            PAGE_PRESENT | PAGE_RW;
    for (uint32_t t = 0; t < KERNEL_FB_TABLES; t++)
        page_directory[(KERNEL_FB_VIRT >> 22) + t] =
            ((uint32_t)(uintptr_t)page_table_fb[t]) | PAGE_PRESENT | PAGE_RW;
    page_directory[KERNEL_MMIO_VIRT >> 22] =
        ((uint32_t)(uintptr_t)page_table_mmio) | PAGE_PRESENT | PAGE_RW;
    page_directory[KERNEL_APIC_MMIO_BASE >> 22] =
        ((uint32_t)(uintptr_t)page_table_apic) | PAGE_PRESENT | PAGE_RW;
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
    if (framebuffer_wc)
        serial_puts("[page] framebuffer PAT write-combining enabled\n");
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
    pd[KERNEL_MMIO_VIRT >> 22] =
        ((uint32_t)(uintptr_t)page_table_mmio) | PAGE_PRESENT | PAGE_RW;
    pd[KERNEL_APIC_MMIO_BASE >> 22] =
        ((uint32_t)(uintptr_t)page_table_apic) | PAGE_PRESENT | PAGE_RW;
    for (uint32_t t = 0; t < KERNEL_LOW_TABLES; t++)
        pd[768 + t] = ((uint32_t)(uintptr_t)page_table_low[t]) |
                      PAGE_PRESENT | PAGE_RW;

    return (uint32_t)(uintptr_t)pd;
}

void *paging_map_mmio(uintptr_t phys_addr, uint32_t size) {
    if (!phys_addr || !size)
        return 0;
    uintptr_t physical_page = phys_addr & ~(uintptr_t)(PAGE_SIZE - 1u);
    uint32_t offset = (uint32_t)(phys_addr - physical_page);
    if (size > 0xFFFFFFFFu - offset)
        return 0;
    uint32_t pages = (size + offset + PAGE_SIZE - 1u) / PAGE_SIZE;
    if (!pages || pages > 1024u)
        return 0;

    paging_lock();
    if (mmio_next_page > 1024u - pages) {
        paging_unlock();
        return 0;
    }
    uint32_t first = mmio_next_page;
    mmio_next_page += pages;
    for (uint32_t i = 0; i < pages; i++) {
        page_table_mmio[first + i] =
            (uint32_t)(physical_page + (uintptr_t)i * PAGE_SIZE) |
            PAGE_PRESENT | PAGE_RW | PAGE_WT | PAGE_CD;
    }
    flush_tlb();
    paging_unlock();
    return (void *)(uintptr_t)(KERNEL_MMIO_VIRT +
                               first * PAGE_SIZE + offset);
}

int paging_map_user_range_in_space(uint32_t cr3, uint32_t va, uint32_t size) {
    uint32_t end;
    if (size == 0)
        return 0;
    if (add_overflows_u32(va, size, &end))
        return -1;
    if (va < USER_SPACE_START || end > USER_SPACE_END)
        return -1;

    uint32_t start = va & ~(PAGE_SIZE - 1u);
    end = (end + PAGE_SIZE - 1u) & ~(PAGE_SIZE - 1u);
    uint32_t *pd = space_page_directory(cr3);
    if (!pd)
        return -1;
    paging_lock();
    for (uint32_t cur = start; cur < end; cur += PAGE_SIZE) {
        uint32_t pde = pde_index(cur);
        uint32_t *user_pt = ensure_user_page_table(pd, pde);
        if (!user_pt) {
            paging_unlock();
            return -1;
        }
        uint32_t idx = pte_index(cur);
        if (user_pt[idx] & PAGE_PRESENT)
            continue;
        uintptr_t phys = pmm_alloc_pages(1);
        if (!phys) {
            paging_unlock();
            return -1;
        }
        uint8_t *page = (uint8_t *)phys;
        for (uint32_t i = 0; i < PAGE_SIZE; i++)
            page[i] = 0;
        user_pt[idx] = (uint32_t)phys | PAGE_PRESENT | PAGE_RW | PAGE_USER;
    }
    if (paging_current_cr3() == cr3)
        flush_tlb();
    paging_unlock();
    return 0;
}

int paging_map_user_range(uint32_t va, uint32_t size) {
    return paging_map_user_range_in_space(paging_current_cr3(), va, size);
}

uint32_t paging_count_user_pages(uint32_t cr3) {
    uint32_t *pd = space_page_directory(cr3);
    uint32_t count = 0;
    if (!pd)
        return 0;
    paging_lock();
    for (uint32_t pde = user_pde_first(); pde <= user_pde_last(); pde++) {
        uint32_t entry = pd[pde];
        if (!(entry & PAGE_PRESENT) || !(entry & PAGE_USER) ||
            (entry & PAGE_LARGE))
            continue;
        uint32_t *pt = (uint32_t *)(uintptr_t)(entry & 0xFFFFF000u);
        for (uint32_t pte = 0; pte < 1024u; pte++) {
            if ((pt[pte] & (PAGE_PRESENT | PAGE_USER)) ==
                (PAGE_PRESENT | PAGE_USER))
                count++;
        }
    }
    paging_unlock();
    return count;
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

int paging_set_user_range_writable_in_space(uint32_t cr3, uint32_t va,
                                            uint32_t size, int writable) {
    if (size == 0)
        return 0;
    uint32_t last;
    if (va < USER_SPACE_START || add_overflows_u32(va, size - 1u, &last) ||
        last >= USER_SPACE_END)
        return -1;

    uint32_t *pd = space_page_directory(cr3);
    if (!pd)
        return -1;
    paging_lock();
    uint32_t cur = va & ~(PAGE_SIZE - 1u);
    uint32_t end_page = last & ~(PAGE_SIZE - 1u);
    for (;;) {
        uint32_t pde = pd[pde_index(cur)];
        if (!(pde & PAGE_PRESENT) || !(pde & PAGE_USER)) {
            paging_unlock();
            return -1;
        }
        uint32_t *pt = (uint32_t *)(uintptr_t)(pde & 0xFFFFF000u);
        uint32_t idx = pte_index(cur);
        if (!(pt[idx] & PAGE_PRESENT) || !(pt[idx] & PAGE_USER)) {
            paging_unlock();
            return -1;
        }
        if (writable)
            pt[idx] |= PAGE_RW;
        else
            pt[idx] &= ~PAGE_RW;
        if (cur == end_page)
            break;
        cur += PAGE_SIZE;
    }
    if (paging_current_cr3() == cr3)
        flush_tlb();
    paging_unlock();
    return 0;
}

int paging_set_user_range_writable(uint32_t va, uint32_t size, int writable) {
    return paging_set_user_range_writable_in_space(
        paging_current_cr3(), va, size, writable);
}

int paging_copy_to_user_space(uint32_t cr3, uint32_t va,
                              const void *src_ptr, uint32_t size) {
    if (size == 0)
        return 0;
    uint32_t last;
    if (!src_ptr || va < USER_SPACE_START ||
        add_overflows_u32(va, size - 1u, &last) || last >= USER_SPACE_END)
        return -1;
    uint32_t *pd = space_page_directory(cr3);
    if (!pd)
        return -1;
    const uint8_t *src = (const uint8_t *)src_ptr;
    paging_lock();
    uint32_t done = 0;
    while (done < size) {
        uint32_t cur = va + done;
        uint32_t pde = pd[pde_index(cur)];
        if (!(pde & PAGE_PRESENT) || !(pde & PAGE_USER)) {
            paging_unlock();
            return -1;
        }
        uint32_t *pt = (uint32_t *)(uintptr_t)(pde & 0xFFFFF000u);
        uint32_t pte = pt[pte_index(cur)];
        if (!(pte & PAGE_PRESENT) || !(pte & PAGE_USER)) {
            paging_unlock();
            return -1;
        }
        uint32_t offset = cur & (PAGE_SIZE - 1u);
        uint32_t chunk = PAGE_SIZE - offset;
        if (chunk > size - done)
            chunk = size - done;
        uint8_t *dst = (uint8_t *)(uintptr_t)(pte & 0xFFFFF000u) + offset;
        for (uint32_t i = 0; i < chunk; i++)
            dst[i] = src[done + i];
        done += chunk;
    }
    paging_unlock();
    return 0;
}

int paging_zero_user_space(uint32_t cr3, uint32_t va, uint32_t size) {
    if (size == 0)
        return 0;
    uint32_t last;
    if (va < USER_SPACE_START ||
        add_overflows_u32(va, size - 1u, &last) || last >= USER_SPACE_END)
        return -1;
    uint32_t *pd = space_page_directory(cr3);
    if (!pd)
        return -1;
    paging_lock();
    uint32_t done = 0;
    while (done < size) {
        uint32_t cur = va + done;
        uint32_t pde = pd[pde_index(cur)];
        if (!(pde & PAGE_PRESENT) || !(pde & PAGE_USER)) {
            paging_unlock();
            return -1;
        }
        uint32_t *pt = (uint32_t *)(uintptr_t)(pde & 0xFFFFF000u);
        uint32_t pte = pt[pte_index(cur)];
        if (!(pte & PAGE_PRESENT) || !(pte & PAGE_USER)) {
            paging_unlock();
            return -1;
        }
        uint32_t offset = cur & (PAGE_SIZE - 1u);
        uint32_t chunk = PAGE_SIZE - offset;
        if (chunk > size - done)
            chunk = size - done;
        uint8_t *dst = (uint8_t *)(uintptr_t)(pte & 0xFFFFF000u) + offset;
        for (uint32_t i = 0; i < chunk; i++)
            dst[i] = 0;
        done += chunk;
    }
    paging_unlock();
    return 0;
}

void paging_destroy_user_space(uint32_t cr3) {
    if (!cr3 || cr3 == paging_kernel_cr3())
        return;

    uint32_t *pd = (uint32_t *)(uintptr_t)cr3;
    paging_lock();
    uint32_t first = user_pde_first();
    uint32_t last = user_pde_last();
    for (uint32_t pde = first; pde <= last; pde++) {
        if (!(pd[pde] & PAGE_PRESENT))
            continue;
        uint32_t *user_pt = (uint32_t *)(uintptr_t)(pd[pde] & 0xFFFFF000u);
        for (uint32_t idx = 0; idx < 1024; idx++) {
            if (user_pt[idx] & PAGE_PRESENT) {
                if (!(user_pt[idx] & PAGE_SHARED))
                    pmm_free_pages((uintptr_t)(user_pt[idx] & 0xFFFFF000u), 1);
                user_pt[idx] = 0;
            }
        }
        pmm_free_pages((uintptr_t)user_pt, 1);
        pd[pde] = 0;
    }

    pmm_free_pages((uintptr_t)pd, 1);
    paging_unlock();
}

int paging_map_user_phys(uint32_t cr3, uint32_t va, uintptr_t phys, uint32_t size,
                         uint32_t pte_flags) {
    if (!size || (va & (PAGE_SIZE - 1u)) || (phys & (PAGE_SIZE - 1u)))
        return -1;
    uint32_t end;
    if (add_overflows_u32(va, size, &end))
        return -1;
    if (va < USER_SPACE_START || end > USER_SPACE_END)
        return -1;
    uint32_t *pd = space_page_directory(cr3);
    if (!pd)
        return -1;
    uint32_t flags = pte_flags | PAGE_PRESENT | PAGE_USER;
    uint32_t pages = (size + PAGE_SIZE - 1u) / PAGE_SIZE;
    paging_lock();
    for (uint32_t i = 0; i < pages; i++) {
        uint32_t cur = va + i * PAGE_SIZE;
        uint32_t *pt = ensure_user_page_table(pd, pde_index(cur));
        if (!pt) {
            paging_unlock();
            return -1;
        }
        uint32_t idx = pte_index(cur);
        /* Replace any prior mapping of this VA (display remap on mode change). */
        pt[idx] = (uint32_t)(phys + i * PAGE_SIZE) | flags;
    }
    if (paging_current_cr3() == cr3)
        flush_tlb();
    paging_unlock();
    return 0;
}

int paging_unmap_user_range(uint32_t cr3, uint32_t va, uint32_t size) {
    if (!size || (va & (PAGE_SIZE - 1u)))
        return -1;
    uint32_t end;
    if (add_overflows_u32(va, size, &end))
        return -1;
    if (va < USER_SPACE_START || end > USER_SPACE_END)
        return -1;
    uint32_t *pd = space_page_directory(cr3);
    if (!pd)
        return -1;
    uint32_t pages = (size + PAGE_SIZE - 1u) / PAGE_SIZE;
    paging_lock();
    for (uint32_t i = 0; i < pages; i++) {
        uint32_t cur = va + i * PAGE_SIZE;
        uint32_t pde = pd[pde_index(cur)];
        if (!(pde & PAGE_PRESENT))
            continue;
        uint32_t *pt = (uint32_t *)(uintptr_t)(pde & 0xFFFFF000u);
        pt[pte_index(cur)] = 0;
    }
    if (paging_current_cr3() == cr3)
        flush_tlb();
    paging_unlock();
    return 0;
}

int paging_map_shared_pages(uint32_t va, const uintptr_t *pages, uint32_t count) {
    if (!pages || !count || (va & (PAGE_SIZE - 1u)))
        return -1;
    uint32_t bytes = count * PAGE_SIZE;
    if (va < USER_SHM_START || bytes > USER_SHM_END - va)
        return -1;
    uint32_t *pd = (uint32_t *)(uintptr_t)paging_current_cr3();
    paging_lock();
    uint32_t mapped = 0;
    for (; mapped < count; mapped++) {
        uint32_t cur = va + mapped * PAGE_SIZE;
        uint32_t *pt = ensure_user_page_table(pd, pde_index(cur));
        if (!pt)
            break;
        uint32_t idx = pte_index(cur);
        if (pt[idx] & PAGE_PRESENT)
            break;
        pt[idx] = (uint32_t)pages[mapped] |
                  PAGE_PRESENT | PAGE_RW | PAGE_USER | PAGE_SHARED;
    }
    if (mapped != count) {
        while (mapped > 0) {
            mapped--;
            uint32_t cur = va + mapped * PAGE_SIZE;
            uint32_t *pt = (uint32_t *)(uintptr_t)(pd[pde_index(cur)] & 0xFFFFF000u);
            pt[pte_index(cur)] = 0;
        }
        flush_tlb();
        paging_unlock();
        return -1;
    }
    flush_tlb();
    paging_unlock();
    return 0;
}

int paging_unmap_shared_pages(uint32_t cr3, uint32_t va, uint32_t count) {
    if (!cr3 || !count || (va & (PAGE_SIZE - 1u)))
        return -1;
    uint32_t *pd = (uint32_t *)(uintptr_t)cr3;
    paging_lock();
    for (uint32_t i = 0; i < count; i++) {
        uint32_t cur = va + i * PAGE_SIZE;
        uint32_t pde = pd[pde_index(cur)];
        if (!(pde & PAGE_PRESENT)) {
            paging_unlock();
            return -1;
        }
        uint32_t *pt = (uint32_t *)(uintptr_t)(pde & 0xFFFFF000u);
        uint32_t idx = pte_index(cur);
        if (!(pt[idx] & PAGE_PRESENT) || !(pt[idx] & PAGE_SHARED)) {
            paging_unlock();
            return -1;
        }
        pt[idx] = 0;
    }
    if (paging_current_cr3() == cr3)
        flush_tlb();
    paging_unlock();
    return 0;
}
