#include "elf.h"
#include "paging.h"
#include "task.h"
#include "user_bounds.h"
#include "vfs.h"

#define ELF_IO_CHUNK 16384u
static uint8_t elf_io_buffer[ELF_IO_CHUNK];

static int range_in_file(uint64_t offset, uint64_t length, size_t size) {
    return offset <= size && length <= (uint64_t)size - offset;
}

static int range_in_load_window(uint64_t address, uint64_t length) {
    if (address < USER_LOAD_START || length > UINT64_MAX - address)
        return 0;
    return address + length <= USER_LOAD_END;
}

static int entry_in_segment(uint64_t entry, const struct elf64_phdr *ph) {
    return ph->p_memsz && ph->p_vaddr <= entry &&
           entry - ph->p_vaddr < ph->p_memsz;
}

static int valid_header(const struct elf64_ehdr *eh, size_t size) {
    if (!eh || size < sizeof(*eh) ||
        eh->e_ident[0] != ELF_MAG0 || eh->e_ident[1] != ELF_MAG1 ||
        eh->e_ident[2] != ELF_MAG2 || eh->e_ident[3] != ELF_MAG3 ||
        eh->e_ident[4] != ELF_CLASS64 || eh->e_ident[5] != ELF_DATA2LSB ||
        eh->e_type != ELF_ET_EXEC || eh->e_machine != ELF_EM_X86_64 ||
        eh->e_version != 1 || eh->e_ehsize != sizeof(*eh) ||
        eh->e_phentsize != sizeof(struct elf64_phdr) || !eh->e_phnum)
        return 0;
    uint64_t bytes = (uint64_t)eh->e_phnum * sizeof(struct elf64_phdr);
    return range_in_file(eh->e_phoff, bytes, size);
}

static int validate_segment(const struct elf64_phdr *ph, size_t size) {
    return ph->p_filesz <= ph->p_memsz &&
           range_in_file(ph->p_offset, ph->p_filesz, size) &&
           range_in_load_window(ph->p_vaddr, ph->p_memsz) &&
           ph->p_memsz <= SIZE_MAX;
}

static int apply_permissions_from_memory(uintptr_t cr3,
                                         const struct elf64_ehdr *eh,
                                         const uint8_t *buf) {
    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        const struct elf64_phdr *ph = (const struct elf64_phdr *)(
            buf + eh->e_phoff + (uint64_t)i * sizeof(*ph));
        if (ph->p_type == PT_LOAD && !(ph->p_flags & PF_W) &&
            paging_set_user_range_writable_in_space(
                cr3, (uintptr_t)ph->p_vaddr, (size_t)ph->p_memsz, 0) < 0)
            return -1;
    }
    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        const struct elf64_phdr *ph = (const struct elf64_phdr *)(
            buf + eh->e_phoff + (uint64_t)i * sizeof(*ph));
        if (ph->p_type == PT_LOAD && (ph->p_flags & PF_W) &&
            paging_set_user_range_writable_in_space(
                cr3, (uintptr_t)ph->p_vaddr, (size_t)ph->p_memsz, 1) < 0)
            return -1;
        if (ph->p_type == PT_LOAD && (ph->p_flags & PF_X) &&
            paging_set_user_range_executable_in_space(
                cr3, (uintptr_t)ph->p_vaddr, (size_t)ph->p_memsz, 1) < 0)
            return -1;
    }
    return 0;
}

uintptr_t elf_load_into_space(uintptr_t cr3, const uint8_t *buf, size_t size,
                              uintptr_t *image_end_out) {
    if (image_end_out) *image_end_out = 0;
    if (!buf || size < sizeof(struct elf64_ehdr)) return 0;
    const struct elf64_ehdr *eh = (const struct elf64_ehdr *)buf;
    if (!valid_header(eh, size)) return 0;

    int saw_load = 0, entry_ok = 0;
    uintptr_t image_end = USER_LOAD_START;
    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        const struct elf64_phdr *ph = (const struct elf64_phdr *)(
            buf + eh->e_phoff + (uint64_t)i * sizeof(*ph));
        if (ph->p_type != PT_LOAD) continue;
        if (!validate_segment(ph, size)) return 0;
        saw_load = 1;
        uintptr_t segment_end = (uintptr_t)(ph->p_vaddr + ph->p_memsz);
        if (segment_end > image_end) image_end = segment_end;
        if (ph->p_memsz && paging_map_user_range_in_space(
                cr3, (uintptr_t)ph->p_vaddr, (size_t)ph->p_memsz) < 0)
            return 0;
        if ((ph->p_flags & PF_X) && entry_in_segment(eh->e_entry, ph))
            entry_ok = 1;
    }
    if (!saw_load || !entry_ok) return 0;

    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        const struct elf64_phdr *ph = (const struct elf64_phdr *)(
            buf + eh->e_phoff + (uint64_t)i * sizeof(*ph));
        if (ph->p_type != PT_LOAD) continue;
        if (paging_copy_to_user_space(cr3, (uintptr_t)ph->p_vaddr,
                                      buf + ph->p_offset, (size_t)ph->p_filesz) < 0 ||
            paging_zero_user_space(cr3, (uintptr_t)(ph->p_vaddr + ph->p_filesz),
                                   (size_t)(ph->p_memsz - ph->p_filesz)) < 0)
            return 0;
    }
    if (apply_permissions_from_memory(cr3, eh, buf) < 0) return 0;
    if (image_end_out) *image_end_out = image_end;
    return (uintptr_t)eh->e_entry;
}

uintptr_t elf_load(const uint8_t *buf, size_t size, uintptr_t *image_end_out) {
    return elf_load_into_space(paging_current_cr3(), buf, size, image_end_out);
}

static int read_exact_at(int fd, uint64_t offset, void *buf, size_t size) {
    if (offset > INT32_MAX || size > INT32_MAX ||
        vfs_lseek(fd, (int)offset, SEEK_SET) < 0) return -1;
    uint8_t *out = (uint8_t *)buf;
    size_t done = 0;
    while (done < size) {
        int n = vfs_read(fd, out + done, size - done);
        if (n <= 0) return -1;
        done += (size_t)n;
    }
    return 0;
}

static int read_phdr(int fd, const struct elf64_ehdr *eh, uint16_t index,
                     struct elf64_phdr *ph) {
    return read_exact_at(fd, eh->e_phoff + (uint64_t)index * sizeof(*ph),
                         ph, sizeof(*ph));
}

uintptr_t elf_load_file_into_space(uintptr_t cr3, int fd, size_t size,
                                   uintptr_t *image_end_out) {
    if (image_end_out) *image_end_out = 0;
    struct elf64_ehdr eh;
    if (fd < 0 || read_exact_at(fd, 0, &eh, sizeof(eh)) < 0 ||
        !valid_header(&eh, size)) return 0;

    int saw_load = 0, entry_ok = 0;
    uintptr_t image_end = USER_LOAD_START;
    for (uint16_t i = 0; i < eh.e_phnum; i++) {
        struct elf64_phdr ph;
        if (read_phdr(fd, &eh, i, &ph) < 0) return 0;
        if (ph.p_type != PT_LOAD) continue;
        if (!validate_segment(&ph, size)) return 0;
        saw_load = 1;
        uintptr_t segment_end = (uintptr_t)(ph.p_vaddr + ph.p_memsz);
        if (segment_end > image_end) image_end = segment_end;
        if (ph.p_memsz && paging_map_user_range_in_space(
                cr3, (uintptr_t)ph.p_vaddr, (size_t)ph.p_memsz) < 0) return 0;
        if ((ph.p_flags & PF_X) && entry_in_segment(eh.e_entry, &ph)) entry_ok = 1;
    }
    if (!saw_load || !entry_ok) return 0;

    for (uint16_t i = 0; i < eh.e_phnum; i++) {
        struct elf64_phdr ph;
        if (read_phdr(fd, &eh, i, &ph) < 0) return 0;
        if (ph.p_type != PT_LOAD) continue;
        size_t copied = 0;
        while (copied < ph.p_filesz) {
            size_t chunk = (size_t)(ph.p_filesz - copied);
            if (chunk > ELF_IO_CHUNK) chunk = ELF_IO_CHUNK;
            if (read_exact_at(fd, ph.p_offset + copied, elf_io_buffer, chunk) < 0 ||
                paging_copy_to_user_space(cr3, (uintptr_t)(ph.p_vaddr + copied),
                                          elf_io_buffer, chunk) < 0) return 0;
            copied += chunk;
            task_yield();
        }
        if (paging_zero_user_space(cr3, (uintptr_t)(ph.p_vaddr + ph.p_filesz),
                                   (size_t)(ph.p_memsz - ph.p_filesz)) < 0) return 0;
    }

    /* File-backed permission pass. */
    for (uint16_t pass = 0; pass < 2; pass++) {
        for (uint16_t i = 0; i < eh.e_phnum; i++) {
            struct elf64_phdr ph;
            if (read_phdr(fd, &eh, i, &ph) < 0) return 0;
            if (ph.p_type != PT_LOAD) continue;
            if (pass == 0 && !(ph.p_flags & PF_W) &&
                paging_set_user_range_writable_in_space(
                    cr3, (uintptr_t)ph.p_vaddr, (size_t)ph.p_memsz, 0) < 0) return 0;
            if (pass == 1 && (ph.p_flags & PF_W) &&
                paging_set_user_range_writable_in_space(
                    cr3, (uintptr_t)ph.p_vaddr, (size_t)ph.p_memsz, 1) < 0) return 0;
            if (pass == 1 && (ph.p_flags & PF_X) &&
                paging_set_user_range_executable_in_space(
                    cr3, (uintptr_t)ph.p_vaddr, (size_t)ph.p_memsz, 1) < 0) return 0;
        }
    }
    if (image_end_out) *image_end_out = image_end;
    return (uintptr_t)eh.e_entry;
}
