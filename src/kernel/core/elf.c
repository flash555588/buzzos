#include "elf.h"
#include "paging.h"
#include "task.h"
#include "user_bounds.h"
#include "vfs.h"

#define ELF_IO_CHUNK 16384u
static uint8_t elf_io_buffer[ELF_IO_CHUNK];

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

uint32_t elf_load_into_space(uint32_t cr3, const uint8_t *buf, size_t size,
                             uint32_t *image_end_out) {
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
        if (paging_map_user_range_in_space(cr3, phdr->p_vaddr,
                                           phdr->p_memsz) < 0)
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

        const uint8_t *src = buf + phdr->p_offset;
        if (paging_copy_to_user_space(cr3, phdr->p_vaddr,
                                      src, phdr->p_filesz) < 0 ||
            paging_zero_user_space(cr3, phdr->p_vaddr + phdr->p_filesz,
                                   phdr->p_memsz - phdr->p_filesz) < 0)
            return 0;
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
            paging_set_user_range_writable_in_space(
                cr3, phdr->p_vaddr, phdr->p_memsz, 0) < 0)
            return 0;
    }
    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        const struct elf32_phdr *phdr =
            (const struct elf32_phdr *)(buf + ehdr->e_phoff + i * sizeof(struct elf32_phdr));
        if (phdr->p_type == PT_LOAD && (phdr->p_flags & PF_W) &&
            paging_set_user_range_writable_in_space(
                cr3, phdr->p_vaddr, phdr->p_memsz, 1) < 0)
            return 0;
    }

    if (image_end_out)
        *image_end_out = image_end;
    return ehdr->e_entry;
}

uint32_t elf_load(const uint8_t *buf, size_t size, uint32_t *image_end_out) {
    return elf_load_into_space(paging_current_cr3(), buf, size, image_end_out);
}

static int read_exact_at(int fd, uint32_t offset, void *buf, uint32_t size) {
    if (offset > 0x7FFFFFFFu || vfs_lseek(fd, (int)offset, SEEK_SET) < 0)
        return -1;
    uint8_t *out = (uint8_t *)buf;
    uint32_t done = 0;
    while (done < size) {
        int n = vfs_read(fd, out + done, size - done);
        if (n <= 0)
            return -1;
        done += (uint32_t)n;
    }
    return 0;
}

static int valid_elf_header(const struct elf32_ehdr *ehdr, size_t size) {
    if (ehdr->e_ident[0] != ELF_MAG0 ||
        ehdr->e_ident[1] != ELF_MAG1 ||
        ehdr->e_ident[2] != ELF_MAG2 ||
        ehdr->e_ident[3] != ELF_MAG3 ||
        ehdr->e_ident[4] != ELF_CLASS32 ||
        ehdr->e_ident[5] != ELF_DATA2LSB ||
        ehdr->e_type != ELF_ET_EXEC ||
        ehdr->e_machine != ELF_EM_386 ||
        ehdr->e_version != 1 ||
        ehdr->e_ehsize != sizeof(struct elf32_ehdr) ||
        ehdr->e_phentsize != sizeof(struct elf32_phdr) ||
        ehdr->e_phnum == 0)
        return 0;
    uint32_t phdr_bytes =
        (uint32_t)ehdr->e_phnum * (uint32_t)sizeof(struct elf32_phdr);
    return file_range_ok(ehdr->e_phoff, phdr_bytes, size);
}

static int read_program_header(int fd, const struct elf32_ehdr *ehdr,
                               uint16_t index, struct elf32_phdr *phdr) {
    uint32_t offset = ehdr->e_phoff +
        (uint32_t)index * (uint32_t)sizeof(struct elf32_phdr);
    return read_exact_at(fd, offset, phdr, sizeof(*phdr));
}

uint32_t elf_load_file_into_space(uint32_t cr3, int fd, size_t size,
                                  uint32_t *image_end_out) {
    if (image_end_out)
        *image_end_out = 0;
    if (fd < 0 || size < sizeof(struct elf32_ehdr))
        return 0;

    struct elf32_ehdr ehdr;
    if (read_exact_at(fd, 0, &ehdr, sizeof(ehdr)) < 0 ||
        !valid_elf_header(&ehdr, size))
        return 0;

    int saw_load = 0;
    int entry_ok = 0;
    uint32_t image_end = USER_LOAD_START;
    for (uint16_t i = 0; i < ehdr.e_phnum; i++) {
        struct elf32_phdr phdr;
        if (read_program_header(fd, &ehdr, i, &phdr) < 0)
            return 0;
        if (phdr.p_type != PT_LOAD)
            continue;
        saw_load = 1;
        if (phdr.p_filesz > phdr.p_memsz ||
            !file_range_ok(phdr.p_offset, phdr.p_filesz, size) ||
            !user_range_ok(phdr.p_vaddr, phdr.p_memsz))
            return 0;
        uint32_t segment_end = phdr.p_vaddr + phdr.p_memsz;
        if (segment_end > image_end)
            image_end = segment_end;
        if (paging_map_user_range_in_space(
                cr3, phdr.p_vaddr, phdr.p_memsz) < 0)
            return 0;
        if ((phdr.p_flags & PF_X) && entry_in_segment(ehdr.e_entry, &phdr))
            entry_ok = 1;
    }
    if (!saw_load || !entry_ok)
        return 0;

    for (uint16_t i = 0; i < ehdr.e_phnum; i++) {
        struct elf32_phdr phdr;
        if (read_program_header(fd, &ehdr, i, &phdr) < 0)
            return 0;
        if (phdr.p_type != PT_LOAD)
            continue;
        if (phdr.p_filesz &&
            vfs_lseek(fd, (int)phdr.p_offset, SEEK_SET) < 0)
            return 0;
        uint32_t copied = 0;
        while (copied < phdr.p_filesz) {
            uint32_t chunk = phdr.p_filesz - copied;
            if (chunk > ELF_IO_CHUNK)
                chunk = ELF_IO_CHUNK;
            int n = vfs_read(fd, elf_io_buffer, chunk);
            if (n <= 0 ||
                paging_copy_to_user_space(cr3, phdr.p_vaddr + copied,
                                          elf_io_buffer, (uint32_t)n) < 0)
                return 0;
            copied += (uint32_t)n;
            task_yield();
        }
        if (paging_zero_user_space(cr3, phdr.p_vaddr + phdr.p_filesz,
                                   phdr.p_memsz - phdr.p_filesz) < 0)
            return 0;
    }

    for (uint16_t i = 0; i < ehdr.e_phnum; i++) {
        struct elf32_phdr phdr;
        if (read_program_header(fd, &ehdr, i, &phdr) < 0)
            return 0;
        if (phdr.p_type == PT_LOAD && !(phdr.p_flags & PF_W) &&
            paging_set_user_range_writable_in_space(
                cr3, phdr.p_vaddr, phdr.p_memsz, 0) < 0)
            return 0;
    }
    for (uint16_t i = 0; i < ehdr.e_phnum; i++) {
        struct elf32_phdr phdr;
        if (read_program_header(fd, &ehdr, i, &phdr) < 0)
            return 0;
        if (phdr.p_type == PT_LOAD && (phdr.p_flags & PF_W) &&
            paging_set_user_range_writable_in_space(
                cr3, phdr.p_vaddr, phdr.p_memsz, 1) < 0)
            return 0;
    }

    if (image_end_out)
        *image_end_out = image_end;
    return ehdr.e_entry;
}
