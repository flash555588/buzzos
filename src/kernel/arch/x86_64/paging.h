#ifndef BUZZOS_X86_64_PAGING_H
#define BUZZOS_X86_64_PAGING_H

#include <stddef.h>
#include <stdint.h>

void paging_init(void);
void paging_set_framebuffer(uintptr_t phys_addr, size_t size);
void *paging_map_mmio(uintptr_t phys_addr, size_t size);
uintptr_t paging_current_cr3(void);
uintptr_t paging_kernel_cr3(void);
void paging_switch(uintptr_t cr3);
uintptr_t paging_create_user_space(void);
void paging_destroy_user_space(uintptr_t cr3);
int paging_map_user_range(uintptr_t va, size_t size);
int paging_map_user_range_in_space(uintptr_t cr3, uintptr_t va, size_t size);
int paging_user_range_accessible(uintptr_t va, size_t size, int write);
int paging_set_user_range_writable(uintptr_t va, size_t size, int writable);
int paging_set_user_range_writable_in_space(uintptr_t cr3, uintptr_t va,
                                            size_t size, int writable);
int paging_set_user_range_executable_in_space(uintptr_t cr3, uintptr_t va,
                                              size_t size, int executable);
int paging_copy_to_user_space(uintptr_t cr3, uintptr_t va,
                              const void *src, size_t size);
int paging_copy_from_user_space(uintptr_t cr3, void *dst, uintptr_t va,
                                size_t size);
int paging_zero_user_space(uintptr_t cr3, uintptr_t va, size_t size);
int paging_map_shared_pages(uintptr_t va, const uintptr_t *pages, size_t count);
int paging_unmap_shared_pages(uintptr_t cr3, uintptr_t va, size_t count);
int paging_map_user_phys(uintptr_t cr3, uintptr_t va, uintptr_t phys,
                         size_t size, uint64_t pte_flags);
int paging_unmap_user_range(uintptr_t cr3, uintptr_t va, size_t size);
int paging_release_user_range(uintptr_t cr3, uintptr_t va, size_t size);
uintptr_t paging_framebuffer_phys(void);
size_t paging_count_user_pages(uintptr_t cr3);

#define PAGE_PRESENT UINT64_C(0x001)
#define PAGE_RW      UINT64_C(0x002)
#define PAGE_USER    UINT64_C(0x004)
#define PAGE_WT      UINT64_C(0x008)
#define PAGE_CD      UINT64_C(0x010)
#define PAGE_LARGE   UINT64_C(0x080)
#define PAGE_SHARED  UINT64_C(0x200)
#define PAGE_NX      UINT64_C(0x8000000000000000)

#define KERNEL_IDENTITY_SIZE UINT64_C(0x100000000)
#define KERNEL_FB_VIRT       UINT64_C(0x0000000010000000)
#define KERNEL_FB_SIZE       UINT64_C(0x0000000004000000)
#define KERNEL_MMIO_VIRT     UINT64_C(0x0000000014000000)
#define KERNEL_MMIO_SIZE     UINT64_C(0x0000000004000000)
#define KERNEL_APIC_MMIO_BASE UINT64_C(0x00000000FEC00000)

#endif
