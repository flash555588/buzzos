#ifndef BUZZOS_ELF_H
#define BUZZOS_ELF_H

#include <stddef.h>
#include <stdint.h>

/* ELF32 header (52 bytes) */
struct elf32_ehdr {
    uint8_t  e_ident[16];   /* 0x00: magic + class + data + version + ... */
    uint16_t e_type;        /* 0x10: 1=reloc, 2=exec, 3=shared, 4=core */
    uint16_t e_machine;     /* 0x12: 3 = EM_386 */
    uint32_t e_version;     /* 0x14 */
    uint32_t e_entry;       /* 0x18: entry point virtual address */
    uint32_t e_phoff;       /* 0x1C: program header table offset */
    uint32_t e_shoff;       /* 0x20: section header table offset */
    uint32_t e_flags;       /* 0x24 */
    uint16_t e_ehsize;      /* 0x28: this header's size */
    uint16_t e_phentsize;   /* 0x2A: program header entry size */
    uint16_t e_phnum;       /* 0x2C: number of program headers */
    uint16_t e_shentsize;   /* 0x2E: section header entry size */
    uint16_t e_shnum;       /* 0x30: number of section headers */
    uint16_t e_shstrndx;    /* 0x32: section header string table index */
} __attribute__((packed));

/* ELF32 program header (32 bytes) */
struct elf32_phdr {
    uint32_t p_type;        /* 1 = PT_LOAD */
    uint32_t p_offset;      /* file offset of segment */
    uint32_t p_vaddr;       /* virtual address to load at */
    uint32_t p_paddr;       /* physical address (ignored on most systems) */
    uint32_t p_filesz;      /* bytes in file image */
    uint32_t p_memsz;       /* bytes in memory image (>= p_filesz) */
    uint32_t p_flags;       /* R=4, W=2, X=1 */
    uint32_t p_align;       /* alignment (0 or 1 = no alignment) */
} __attribute__((packed));

enum {
    ELF_MAG0   = 0x7F,
    ELF_MAG1   = 'E',
    ELF_MAG2   = 'L',
    ELF_MAG3   = 'F',
    ELF_CLASS32 = 1,
    ELF_DATA2LSB = 1,
    ELF_ET_EXEC = 2,
    ELF_EM_386  = 3,
    PT_LOAD     = 1,
    PF_R        = 4,
    PF_W        = 2,
    PF_X        = 1,
};

/* Parse, validate, and load an ELF image from `buf`. Returns the entry point
 * virtual address, or 0 on error. */
uint32_t elf_load(const uint8_t *buf, size_t size, uint32_t *image_end_out);

#endif /* BUZZOS_ELF_H */
