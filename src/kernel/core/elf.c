#include "elf.h"
#include "paging.h"
#include "user_bounds.h"

static int add_overflows_u32(uint32_t a, uint32_t b, uint32_t *out) {
    if (b > 0xFFFFFFFFu - a)
        return 1;
    *out = a + b;
    return 0;
}

static int file_range_ok(uint32_t offset, uint32_t len, size_t size) {
    if ((size_t)offset > size)
        return 0;
    if ((size_t)len > size - (size_t)offset)
        return 0;
    return 1;
}

static int user_range_ok(uint32_t addr, uint32_t len) {
    uint32_t end;
    if (addr < USER_LOAD_START)
        return 0;
    if (add_overflows_u32(addr, len, &end))
        return 0;
    return end <= USER_LOAD_END;
}

static int entry_in_segment(uint32_t entry, const struct elf32_phdr *phdr) {
    uint32_t end;
    if (phdr->p_memsz == 0)
        return 0;
    if (add_overflows_u32(phdr->p_vaddr, phdr->p_memsz, &end))
        return 0;
    return entry >= phdr->p_vaddr && entry < end;
}

uint32_t elf_load(const uint8_t *buf, size_t size, uint32_t *image_end_out) {
    if (image_end_out)
        *image_end_out = 0;
    if (!buf || size < sizeof(struct elf32_ehdr))
        return 0;

    const struct elf32_ehdr *ehdr = (const struct elf32_ehdr *)buf;

    /* Verify ELF magic */
    if (ehdr->e_ident[0] != ELF_MAG0 ||
        ehdr->e_ident[1] != ELF_MAG1 ||
        ehdr->e_ident[2] != ELF_MAG2 ||
        ehdr->e_ident[3] != ELF_MAG3)
        return 0;

    /* 32-bit, little-endian, executable */
    if (ehdr->e_ident[4] != ELF_CLASS32) return 0;
    if (ehdr->e_ident[5] != ELF_DATA2LSB) return 0;
    if (ehdr->e_type != ELF_ET_EXEC) return 0;
    if (ehdr->e_machine != ELF_EM_386) return 0;
    if (ehdr->e_version != 1) return 0;
    if (ehdr->e_ehsize != sizeof(struct elf32_ehdr)) return 0;
    if (ehdr->e_phentsize != sizeof(struct elf32_phdr)) return 0;
    if (ehdr->e_phnum == 0) return 0;

    uint32_t phdr_bytes = (uint32_t)ehdr->e_phnum * (uint32_t)sizeof(struct elf32_phdr);
    if (!file_range_ok(ehdr->e_phoff, phdr_bytes, size))
        return 0;

    int saw_load = 0;
    int entry_ok = 0;
    uint32_t image_end = USER_LOAD_START;

    /* Validate all loadable segments before writing anything. */
    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        const struct elf32_phdr *phdr =
            (const struct elf32_phdr *)(buf + ehdr->e_phoff + i * sizeof(struct elf32_phdr));

        if (phdr->p_type != PT_LOAD)
            continue;

        saw_load = 1;
        if (phdr->p_filesz > phdr->p_memsz)
            return 0;
        if (!file_range_ok(phdr->p_offset, phdr->p_filesz, size))
            return 0;
        if (!user_range_ok(phdr->p_vaddr, phdr->p_memsz))
            return 0;
        uint32_t segment_end = phdr->p_vaddr + phdr->p_memsz;
        if (segment_end > image_end)
            image_end = segment_end;
        if (paging_map_user_range(phdr->p_vaddr, phdr->p_memsz) < 0)
            return 0;
        if ((phdr->p_flags & PF_X) && entry_in_segment(ehdr->e_entry, phdr))
            entry_ok = 1;
    }

    if (!saw_load || !entry_ok)
        return 0;

    /* Load each PT_LOAD segment */
    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        const struct elf32_phdr *phdr =
            (const struct elf32_phdr *)(buf + ehdr->e_phoff + i * sizeof(struct elf32_phdr));

        if (phdr->p_type != PT_LOAD) continue;

        uint8_t *dest = (uint8_t *)(uintptr_t)phdr->p_vaddr;
        const uint8_t *src = buf + phdr->p_offset;

        /* Copy file data */
        for (uint32_t j = 0; j < phdr->p_filesz; j++) {
            dest[j] = src[j];
        }

        /* Zero-fill .bss portion */
        for (uint32_t j = phdr->p_filesz; j < phdr->p_memsz; j++) {
            dest[j] = 0;
        }
    }

    /* Apply final write permissions only after relocation/copying is done.
     * ELF segments need not begin on separate pages. First restrict every
     * page covered by a non-writable segment, then restore pages covered by
     * writable segments. The second pass implements the required page-level
     * permission union for overlapping PT_LOAD ranges.
     *
     * i386 without PAE/NX cannot enforce execute-disable, but read-only code
     * pages still prevent user processes from modifying their instructions. */
    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        const struct elf32_phdr *phdr =
            (const struct elf32_phdr *)(buf + ehdr->e_phoff + i * sizeof(struct elf32_phdr));
        if (phdr->p_type == PT_LOAD && !(phdr->p_flags & PF_W) &&
            paging_set_user_range_writable(phdr->p_vaddr, phdr->p_memsz, 0) < 0)
            return 0;
    }
    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        const struct elf32_phdr *phdr =
            (const struct elf32_phdr *)(buf + ehdr->e_phoff + i * sizeof(struct elf32_phdr));
        if (phdr->p_type == PT_LOAD && (phdr->p_flags & PF_W) &&
            paging_set_user_range_writable(phdr->p_vaddr, phdr->p_memsz, 1) < 0)
            return 0;
    }

    if (image_end_out)
        *image_end_out = image_end;
    return ehdr->e_entry;
}
