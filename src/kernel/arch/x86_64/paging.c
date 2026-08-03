#include "paging.h"
#include "pmm.h"
#include "serial.h"
#include "task.h"
#include "user_bounds.h"

#define PT_ENTRIES 512u
#define ENTRY_ADDR_MASK UINT64_C(0x000FFFFFFFFFF000)
#define TWO_MIB UINT64_C(0x200000)
#define FB_TABLES ((size_t)(KERNEL_FB_SIZE / TWO_MIB))
#define MMIO_TABLES ((size_t)(KERNEL_MMIO_SIZE / TWO_MIB))

typedef uint64_t pte_t;

static pte_t kernel_pml4[PT_ENTRIES] __attribute__((aligned(4096)));
static pte_t kernel_pdpt[PT_ENTRIES] __attribute__((aligned(4096)));
static pte_t kernel_pd[4][PT_ENTRIES] __attribute__((aligned(4096)));
static pte_t framebuffer_pts[FB_TABLES][PT_ENTRIES] __attribute__((aligned(4096)));
static pte_t mmio_pts[MMIO_TABLES][PT_ENTRIES] __attribute__((aligned(4096)));

static uintptr_t framebuffer_phys = UINT64_C(0xE0000000);
static size_t framebuffer_size = KERNEL_FB_SIZE;
static size_t mmio_next_page;
static volatile int paging_locked;

static inline uintptr_t entry_address(pte_t entry) {
    return (uintptr_t)(entry & ENTRY_ADDR_MASK);
}

static void zero_table(pte_t *table) {
    for (size_t i = 0; i < PT_ENTRIES; i++)
        table[i] = 0;
}

static void paging_lock(void) {
    while (__sync_lock_test_and_set(&paging_locked, 1))
        task_yield();
}

static void paging_unlock(void) {
    __sync_lock_release(&paging_locked);
}

static void flush_tlb(void) {
    uintptr_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
}

static int add_overflows(uintptr_t base, size_t size, uintptr_t *end) {
    if (size > UINTPTR_MAX - base)
        return 1;
    *end = base + size;
    return 0;
}

static pte_t *alloc_table(void) {
    uintptr_t phys = pmm_alloc_pages(1);
    if (!phys)
        return 0;
    pte_t *table = (pte_t *)phys;
    zero_table(table);
    return table;
}

static pte_t *next_table(pte_t *table, size_t index, int create, int user) {
    pte_t entry = table[index];
    if (entry & PAGE_PRESENT) {
        if (entry & PAGE_LARGE)
            return 0;
        if (user && !(entry & PAGE_USER))
            table[index] |= PAGE_USER;
        return (pte_t *)entry_address(entry);
    }
    if (!create)
        return 0;
    pte_t *next = alloc_table();
    if (!next)
        return 0;
    table[index] = (pte_t)(uintptr_t)next | PAGE_PRESENT | PAGE_RW |
                   (user ? PAGE_USER : 0);
    return next;
}

static pte_t *walk_pte(uintptr_t cr3, uintptr_t va, int create, int user) {
    pte_t *pml4 = (pte_t *)(cr3 & ~(uintptr_t)(PAGE_SIZE - 1u));
    if (!pml4)
        return 0;
    size_t i4 = (size_t)((va >> 39) & 0x1FFu);
    size_t i3 = (size_t)((va >> 30) & 0x1FFu);
    size_t i2 = (size_t)((va >> 21) & 0x1FFu);
    size_t i1 = (size_t)((va >> 12) & 0x1FFu);
    pte_t *pdpt = next_table(pml4, i4, create, user);
    if (!pdpt) return 0;
    pte_t *pd = next_table(pdpt, i3, create, user);
    if (!pd) return 0;
    pte_t *pt = next_table(pd, i2, create, user);
    if (!pt) return 0;
    return &pt[i1];
}

static int user_bounds(uintptr_t va, size_t size, uintptr_t *end) {
    if (!size || va < USER_SPACE_START || add_overflows(va, size, end))
        return 0;
    return *end <= USER_SPACE_END;
}

uintptr_t paging_framebuffer_phys(void) {
    return framebuffer_phys;
}

void paging_set_framebuffer(uintptr_t phys_addr, size_t size) {
    if (!phys_addr || !size)
        return;
    framebuffer_phys = phys_addr & ~(uintptr_t)(PAGE_SIZE - 1u);
    framebuffer_size = size > KERNEL_FB_SIZE ? KERNEL_FB_SIZE : size;
}

static int configure_pat(void) {
    uint32_t eax = 1, ebx, ecx, edx;
    __asm__ volatile("cpuid" : "+a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx));
    if (!(edx & (1u << 16)))
        return 0;
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0x277u));
    lo = (lo & 0x00FFFFFFu) | (1u << 24);
    __asm__ volatile("wrmsr" : : "c"(0x277u), "a"(lo), "d"(hi));
    return 1;
}

void paging_init(void) {
    serial_puts("[page] installing x86_64 four-level paging...\n");
    int framebuffer_wc = configure_pat();
    zero_table(kernel_pml4);
    zero_table(kernel_pdpt);
    for (size_t n = 0; n < 4; n++)
        zero_table(kernel_pd[n]);
    for (size_t n = 0; n < FB_TABLES; n++)
        zero_table(framebuffer_pts[n]);
    for (size_t n = 0; n < MMIO_TABLES; n++)
        zero_table(mmio_pts[n]);

    kernel_pml4[0] = (pte_t)(uintptr_t)kernel_pdpt | PAGE_PRESENT | PAGE_RW;
    for (size_t n = 0; n < 4; n++) {
        kernel_pdpt[n] = (pte_t)(uintptr_t)kernel_pd[n] | PAGE_PRESENT | PAGE_RW;
        for (size_t i = 0; i < PT_ENTRIES; i++) {
            uint64_t phys = ((uint64_t)n * PT_ENTRIES + i) * TWO_MIB;
            kernel_pd[n][i] = phys | PAGE_PRESENT | PAGE_RW | PAGE_LARGE;
        }
    }

    size_t fb_pd = (size_t)(KERNEL_FB_VIRT >> 21);
    for (size_t t = 0; t < FB_TABLES; t++) {
        for (size_t i = 0; i < PT_ENTRIES; i++) {
            size_t offset = (t * PT_ENTRIES + i) * PAGE_SIZE;
            if (offset < framebuffer_size)
                framebuffer_pts[t][i] = (pte_t)(framebuffer_phys + offset) |
                    PAGE_PRESENT | PAGE_RW | PAGE_WT | PAGE_CD;
        }
        kernel_pd[0][fb_pd + t] = (pte_t)(uintptr_t)framebuffer_pts[t] |
                                  PAGE_PRESENT | PAGE_RW;
    }

    size_t mmio_pd = (size_t)(KERNEL_MMIO_VIRT >> 21);
    for (size_t t = 0; t < MMIO_TABLES; t++)
        kernel_pd[0][mmio_pd + t] = (pte_t)(uintptr_t)mmio_pts[t] |
                                    PAGE_PRESENT | PAGE_RW;

    /* APIC pages are identity mapped but uncached. */
    for (uintptr_t va = KERNEL_APIC_MMIO_BASE;
         va < KERNEL_APIC_MMIO_BASE + UINT64_C(0x400000); va += TWO_MIB) {
        size_t pdpt = (size_t)((va >> 30) & 3u);
        size_t pd = (size_t)((va >> 21) & 0x1FFu);
        kernel_pd[pdpt][pd] = (pte_t)va | PAGE_PRESENT | PAGE_RW |
                              PAGE_LARGE | PAGE_WT | PAGE_CD;
    }

    paging_switch((uintptr_t)kernel_pml4);
    uint64_t cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= UINT64_C(0x00010000);     /* supervisor write-protect */
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0) : "memory");

    serial_puts("[page] x86_64 paging active\n");
    if (framebuffer_wc)
        serial_puts("[page] framebuffer PAT write-combining enabled\n");
}

uintptr_t paging_current_cr3(void) {
    uintptr_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    return cr3 & ~(uintptr_t)(PAGE_SIZE - 1u);
}

uintptr_t paging_kernel_cr3(void) {
    return (uintptr_t)kernel_pml4;
}

void paging_switch(uintptr_t cr3) {
    __asm__ volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
}

uintptr_t paging_create_user_space(void) {
    pte_t *pml4 = alloc_table();
    pte_t *pdpt = alloc_table();
    if (!pml4 || !pdpt) {
        if (pml4) pmm_free_pages((uintptr_t)pml4, 1);
        if (pdpt) pmm_free_pages((uintptr_t)pdpt, 1);
        return 0;
    }
    pml4[0] = (pte_t)(uintptr_t)pdpt | PAGE_PRESENT | PAGE_RW;
    for (size_t i = 0; i < 4; i++)
        pdpt[i] = kernel_pdpt[i];
    return (uintptr_t)pml4;
}

void *paging_map_mmio(uintptr_t phys_addr, size_t size) {
    if (!phys_addr || !size)
        return 0;
    uintptr_t physical_page = phys_addr & ~(uintptr_t)(PAGE_SIZE - 1u);
    size_t offset = (size_t)(phys_addr - physical_page);
    if (size > SIZE_MAX - offset)
        return 0;
    size_t pages = (size + offset + PAGE_SIZE - 1u) / PAGE_SIZE;
    size_t capacity = MMIO_TABLES * PT_ENTRIES;
    paging_lock();
    if (!pages || pages > capacity - mmio_next_page) {
        paging_unlock();
        return 0;
    }
    size_t first = mmio_next_page;
    mmio_next_page += pages;
    for (size_t i = 0; i < pages; i++) {
        size_t slot = first + i;
        mmio_pts[slot / PT_ENTRIES][slot % PT_ENTRIES] =
            (pte_t)(physical_page + i * PAGE_SIZE) |
            PAGE_PRESENT | PAGE_RW | PAGE_WT | PAGE_CD;
    }
    flush_tlb();
    paging_unlock();
    return (void *)(KERNEL_MMIO_VIRT + first * PAGE_SIZE + offset);
}

int paging_map_user_range_in_space(uintptr_t cr3, uintptr_t va, size_t size) {
    uintptr_t end;
    if (!user_bounds(va, size, &end))
        return -1;
    uintptr_t cur = va & ~(uintptr_t)(PAGE_SIZE - 1u);
    end = (end + PAGE_SIZE - 1u) & ~(uintptr_t)(PAGE_SIZE - 1u);
    paging_lock();
    for (; cur < end; cur += PAGE_SIZE) {
        pte_t *pte = walk_pte(cr3, cur, 1, 1);
        if (!pte) { paging_unlock(); return -1; }
        if (*pte & PAGE_PRESENT)
            continue;
        uintptr_t phys = pmm_alloc_pages(1);
        if (!phys) { paging_unlock(); return -1; }
        uint8_t *page = (uint8_t *)phys;
        for (size_t i = 0; i < PAGE_SIZE; i++) page[i] = 0;
        *pte = (pte_t)phys | PAGE_PRESENT | PAGE_RW | PAGE_USER | PAGE_NX;
    }
    if (paging_current_cr3() == cr3) flush_tlb();
    paging_unlock();
    return 0;
}

int paging_map_user_range(uintptr_t va, size_t size) {
    return paging_map_user_range_in_space(paging_current_cr3(), va, size);
}

static int pte_for_user(uintptr_t cr3, uintptr_t va, int write, pte_t **out) {
    pte_t *pte = walk_pte(cr3, va, 0, 1);
    if (!pte || !(*pte & PAGE_PRESENT) || !(*pte & PAGE_USER) ||
        (write && !(*pte & PAGE_RW)))
        return 0;
    *out = pte;
    return 1;
}

int paging_user_range_accessible(uintptr_t va, size_t size, int write) {
    if (!size) return 1;
    uintptr_t end;
    if (!user_bounds(va, size, &end)) return 0;
    uintptr_t last = end - 1u;
    for (uintptr_t cur = va & ~(uintptr_t)(PAGE_SIZE - 1u);;
         cur += PAGE_SIZE) {
        pte_t *pte;
        if (!pte_for_user(paging_current_cr3(), cur, write, &pte)) return 0;
        if (cur == (last & ~(uintptr_t)(PAGE_SIZE - 1u))) break;
    }
    return 1;
}

static int set_user_flag(uintptr_t cr3, uintptr_t va, size_t size,
                         pte_t flag, int enabled) {
    uintptr_t end;
    if (!user_bounds(va, size, &end)) return -1;
    uintptr_t last = end - 1u;
    paging_lock();
    for (uintptr_t cur = va & ~(uintptr_t)(PAGE_SIZE - 1u);;
         cur += PAGE_SIZE) {
        pte_t *pte;
        if (!pte_for_user(cr3, cur, 0, &pte)) { paging_unlock(); return -1; }
        if (enabled) *pte |= flag; else *pte &= ~flag;
        if (cur == (last & ~(uintptr_t)(PAGE_SIZE - 1u))) break;
    }
    if (paging_current_cr3() == cr3) flush_tlb();
    paging_unlock();
    return 0;
}

int paging_set_user_range_writable_in_space(uintptr_t cr3, uintptr_t va,
                                            size_t size, int writable) {
    return set_user_flag(cr3, va, size, PAGE_RW, writable);
}

int paging_set_user_range_writable(uintptr_t va, size_t size, int writable) {
    return paging_set_user_range_writable_in_space(
        paging_current_cr3(), va, size, writable);
}

int paging_set_user_range_executable_in_space(uintptr_t cr3, uintptr_t va,
                                              size_t size, int executable) {
    return set_user_flag(cr3, va, size, PAGE_NX, !executable);
}

int paging_copy_to_user_space(uintptr_t cr3, uintptr_t va,
                              const void *src_ptr, size_t size) {
    uintptr_t end;
    if (!src_ptr || !user_bounds(va, size, &end)) return size ? -1 : 0;
    const uint8_t *src = (const uint8_t *)src_ptr;
    paging_lock();
    size_t done = 0;
    while (done < size) {
        uintptr_t cur = va + done;
        pte_t *pte;
        if (!pte_for_user(cr3, cur, 0, &pte)) { paging_unlock(); return -1; }
        size_t offset = (size_t)(cur & (PAGE_SIZE - 1u));
        size_t chunk = PAGE_SIZE - offset;
        if (chunk > size - done) chunk = size - done;
        uint8_t *dst = (uint8_t *)entry_address(*pte) + offset;
        for (size_t i = 0; i < chunk; i++) dst[i] = src[done + i];
        done += chunk;
    }
    paging_unlock();
    return 0;
}

int paging_zero_user_space(uintptr_t cr3, uintptr_t va, size_t size) {
    uintptr_t end;
    if (!user_bounds(va, size, &end)) return size ? -1 : 0;
    paging_lock();
    size_t done = 0;
    while (done < size) {
        uintptr_t cur = va + done;
        pte_t *pte;
        if (!pte_for_user(cr3, cur, 0, &pte)) { paging_unlock(); return -1; }
        size_t offset = (size_t)(cur & (PAGE_SIZE - 1u));
        size_t chunk = PAGE_SIZE - offset;
        if (chunk > size - done) chunk = size - done;
        uint8_t *dst = (uint8_t *)entry_address(*pte) + offset;
        for (size_t i = 0; i < chunk; i++) dst[i] = 0;
        done += chunk;
    }
    paging_unlock();
    return 0;
}

int paging_map_user_phys(uintptr_t cr3, uintptr_t va, uintptr_t phys,
                         size_t size, uint64_t pte_flags) {
    uintptr_t end;
    if (!size || (va & (PAGE_SIZE - 1u)) || (phys & (PAGE_SIZE - 1u)) ||
        !user_bounds(va, size, &end)) return -1;
    size_t pages = (size + PAGE_SIZE - 1u) / PAGE_SIZE;
    paging_lock();
    for (size_t i = 0; i < pages; i++) {
        pte_t *pte = walk_pte(cr3, va + i * PAGE_SIZE, 1, 1);
        if (!pte) { paging_unlock(); return -1; }
        *pte = (pte_t)(phys + i * PAGE_SIZE) | pte_flags | PAGE_PRESENT | PAGE_USER;
    }
    if (paging_current_cr3() == cr3) flush_tlb();
    paging_unlock();
    return 0;
}

static int unmap_range(uintptr_t cr3, uintptr_t va, size_t size, int release) {
    uintptr_t end;
    if (!size || (va & (PAGE_SIZE - 1u)) || !user_bounds(va, size, &end))
        return -1;
    size_t pages = (size + PAGE_SIZE - 1u) / PAGE_SIZE;
    paging_lock();
    for (size_t i = 0; i < pages; i++) {
        pte_t *pte = walk_pte(cr3, va + i * PAGE_SIZE, 0, 1);
        if (!pte || !(*pte & PAGE_PRESENT)) continue;
        if (release && !(*pte & PAGE_SHARED))
            pmm_free_pages(entry_address(*pte), 1);
        *pte = 0;
    }
    if (paging_current_cr3() == cr3) flush_tlb();
    paging_unlock();
    return 0;
}

int paging_unmap_user_range(uintptr_t cr3, uintptr_t va, size_t size) {
    return unmap_range(cr3, va, size, 0);
}

int paging_release_user_range(uintptr_t cr3, uintptr_t va, size_t size) {
    return unmap_range(cr3, va, size, 1);
}

int paging_map_shared_pages(uintptr_t va, const uintptr_t *pages, size_t count) {
    if (!pages || !count || (va & (PAGE_SIZE - 1u)) ||
        va < USER_SHM_START || count > (USER_SHM_END - va) / PAGE_SIZE)
        return -1;
    uintptr_t cr3 = paging_current_cr3();
    paging_lock();
    size_t mapped = 0;
    for (; mapped < count; mapped++) {
        pte_t *pte = walk_pte(cr3, va + mapped * PAGE_SIZE, 1, 1);
        if (!pte || (*pte & PAGE_PRESENT)) break;
        *pte = (pte_t)pages[mapped] | PAGE_PRESENT | PAGE_RW |
               PAGE_USER | PAGE_SHARED | PAGE_NX;
    }
    if (mapped != count) {
        while (mapped) {
            mapped--;
            pte_t *pte = walk_pte(cr3, va + mapped * PAGE_SIZE, 0, 1);
            if (pte) *pte = 0;
        }
        flush_tlb(); paging_unlock(); return -1;
    }
    flush_tlb(); paging_unlock(); return 0;
}

int paging_unmap_shared_pages(uintptr_t cr3, uintptr_t va, size_t count) {
    if (!cr3 || !count || (va & (PAGE_SIZE - 1u))) return -1;
    paging_lock();
    for (size_t i = 0; i < count; i++) {
        pte_t *pte = walk_pte(cr3, va + i * PAGE_SIZE, 0, 1);
        if (!pte || !(*pte & PAGE_PRESENT) || !(*pte & PAGE_SHARED)) {
            paging_unlock(); return -1;
        }
        *pte = 0;
    }
    if (paging_current_cr3() == cr3) flush_tlb();
    paging_unlock(); return 0;
}

size_t paging_count_user_pages(uintptr_t cr3) {
    pte_t *pml4 = (pte_t *)cr3;
    size_t count = 0;
    if (!pml4) return 0;
    paging_lock();
    for (size_t i4 = 0; i4 < PT_ENTRIES; i4++) {
        if (!(pml4[i4] & PAGE_PRESENT)) continue;
        pte_t *pdpt = (pte_t *)entry_address(pml4[i4]);
        for (size_t i3 = 0; i3 < PT_ENTRIES; i3++) {
            if (!(pdpt[i3] & PAGE_PRESENT) || !(pdpt[i3] & PAGE_USER)) continue;
            pte_t *pd = (pte_t *)entry_address(pdpt[i3]);
            for (size_t i2 = 0; i2 < PT_ENTRIES; i2++) {
                if (!(pd[i2] & PAGE_PRESENT) || !(pd[i2] & PAGE_USER) ||
                    (pd[i2] & PAGE_LARGE)) continue;
                pte_t *pt = (pte_t *)entry_address(pd[i2]);
                for (size_t i1 = 0; i1 < PT_ENTRIES; i1++)
                    if ((pt[i1] & (PAGE_PRESENT | PAGE_USER)) ==
                        (PAGE_PRESENT | PAGE_USER)) count++;
            }
        }
    }
    paging_unlock();
    return count;
}

static int is_kernel_table(uintptr_t phys) {
    if (phys == (uintptr_t)kernel_pdpt) return 1;
    for (size_t i = 0; i < 4; i++) if (phys == (uintptr_t)kernel_pd[i]) return 1;
    for (size_t i = 0; i < FB_TABLES; i++) if (phys == (uintptr_t)framebuffer_pts[i]) return 1;
    for (size_t i = 0; i < MMIO_TABLES; i++) if (phys == (uintptr_t)mmio_pts[i]) return 1;
    return 0;
}

void paging_destroy_user_space(uintptr_t cr3) {
    if (!cr3 || cr3 == paging_kernel_cr3()) return;
    pte_t *pml4 = (pte_t *)cr3;
    paging_lock();
    for (size_t i4 = 0; i4 < PT_ENTRIES; i4++) {
        if (!(pml4[i4] & PAGE_PRESENT)) continue;
        uintptr_t pdpt_phys = entry_address(pml4[i4]);
        pte_t *pdpt = (pte_t *)pdpt_phys;
        for (size_t i3 = 0; i3 < PT_ENTRIES; i3++) {
            if (!(pdpt[i3] & PAGE_PRESENT) || is_kernel_table(entry_address(pdpt[i3]))) continue;
            uintptr_t pd_phys = entry_address(pdpt[i3]);
            pte_t *pd = (pte_t *)pd_phys;
            for (size_t i2 = 0; i2 < PT_ENTRIES; i2++) {
                if (!(pd[i2] & PAGE_PRESENT) || (pd[i2] & PAGE_LARGE) ||
                    is_kernel_table(entry_address(pd[i2]))) continue;
                uintptr_t pt_phys = entry_address(pd[i2]);
                pte_t *pt = (pte_t *)pt_phys;
                for (size_t i1 = 0; i1 < PT_ENTRIES; i1++) {
                    if ((pt[i1] & PAGE_PRESENT) && !(pt[i1] & PAGE_SHARED))
                        pmm_free_pages(entry_address(pt[i1]), 1);
                }
                pmm_free_pages(pt_phys, 1);
            }
            pmm_free_pages(pd_phys, 1);
        }
        if (!is_kernel_table(pdpt_phys)) pmm_free_pages(pdpt_phys, 1);
    }
    pmm_free_pages(cr3, 1);
    paging_unlock();
}
