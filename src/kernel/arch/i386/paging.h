#ifndef BUZZOS_PAGING_H
#define BUZZOS_PAGING_H

#include <stdint.h>

/* Set up a page directory and page tables:
 *   - Identity-map 0x00000000 -> 0x00000000 (low managed memory)
 *   - Map 0xC0000000 -> 0x00000000 (low managed memory alias)
 * Then enable paging (CR0.PG) and jump-call to flush the TLB.
 * After this call, virtual == physical for the managed low-memory window. */
void paging_init(void);
void paging_set_framebuffer(uintptr_t phys_addr, uint32_t size);
uint32_t paging_current_cr3(void);
uint32_t paging_kernel_cr3(void);
void paging_switch(uint32_t cr3);
uint32_t paging_create_user_space(void);
void paging_destroy_user_space(uint32_t cr3);
int paging_map_user_range(uint32_t va, uint32_t size);
int paging_user_range_accessible(uint32_t va, uint32_t size, int write);
int paging_set_user_range_writable(uint32_t va, uint32_t size, int writable);
int paging_map_shared_pages(uint32_t va, const uintptr_t *pages, uint32_t count);
int paging_unmap_shared_pages(uint32_t cr3, uint32_t va, uint32_t count);

/* Page directory and table entry flags */
enum {
    PAGE_PRESENT  = 0x01,
    PAGE_RW       = 0x02,
    PAGE_USER     = 0x04,
    PAGE_WT       = 0x08,
    PAGE_CD       = 0x10,
    PAGE_LARGE    = 0x80,   /* 4 MiB page (PDE only) */
    PAGE_SHARED   = 0x200,  /* physical page owned by a shared-memory object */
};

enum {
    KERNEL_LOW_TABLES = 64u,
    KERNEL_LOW_SIZE = KERNEL_LOW_TABLES * 0x00400000u,
    KERNEL_FB_VIRT = 0x10000000u,
    KERNEL_FB_TABLES = 4u,
    KERNEL_FB_SIZE = KERNEL_FB_TABLES * 0x00400000u,
};

#endif /* BUZZOS_PAGING_H */
