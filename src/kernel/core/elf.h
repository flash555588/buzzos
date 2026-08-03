#ifndef BUZZOS_ELF_H
#define BUZZOS_ELF_H

#include <stddef.h>
#include <stdint.h>

struct elf64_ehdr {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} __attribute__((packed));

struct elf64_phdr {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} __attribute__((packed));

enum {
    ELF_MAG0 = 0x7F, ELF_MAG1 = 'E', ELF_MAG2 = 'L', ELF_MAG3 = 'F',
    ELF_CLASS64 = 2, ELF_DATA2LSB = 1, ELF_ET_EXEC = 2,
    ELF_EM_X86_64 = 62, PT_LOAD = 1,
    PF_X = 1, PF_W = 2, PF_R = 4,
};

uintptr_t elf_load(const uint8_t *buf, size_t size, uintptr_t *image_end_out);
uintptr_t elf_load_into_space(uintptr_t cr3, const uint8_t *buf, size_t size,
                              uintptr_t *image_end_out);
uintptr_t elf_load_file_into_space(uintptr_t cr3, int fd, size_t size,
                                   uintptr_t *image_end_out);

#endif
