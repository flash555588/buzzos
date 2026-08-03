; Multiboot2 enters an x86 machine in 32-bit protected mode.  This is the
; only 32-bit code in BuzzOS: it builds an identity map for the first 4 GiB,
; enables long mode, and transfers permanently to the x86_64 kernel.

bits 32

global _start
extern kernel_main
extern __boot_stack_top

section .multiboot
align 8
mb2_header_start:
    dd 0xE85250D6
    dd 0                         ; Multiboot2 i386 entry protocol
    dd mb2_header_end - mb2_header_start
    dd -(0xE85250D6 + 0 + (mb2_header_end - mb2_header_start))

    ; Request a linear 1600x900x32 framebuffer.
    dw 5
    dw 0
    dd 20
    dd 1600
    dd 900
    dd 32

align 8
    dw 0
    dw 0
    dd 8
mb2_header_end:

section .text.entry
_start:
    cli
    mov ebp, eax                 ; preserve Multiboot magic
    mov esi, ebx                 ; preserve Multiboot information pointer
    mov esp, __boot_stack_top

    ; Clear the six bootstrap paging pages.
    mov eax, boot_pml4
    mov ecx, (6 * 4096) / 4
.clear_tables:
    mov dword [eax], 0
    add eax, 4
    loop .clear_tables

    ; PML4[0] -> PDPT, PDPT[0..3] -> four page directories.
    mov eax, boot_pdpt
    or eax, 0x03
    mov dword [boot_pml4], eax
    mov dword [boot_pml4 + 4], 0

    mov eax, boot_pd
    or eax, 0x03
    mov dword [boot_pdpt + 0], eax
    mov dword [boot_pdpt + 4], 0
    mov eax, boot_pd + 4096
    or eax, 0x03
    mov dword [boot_pdpt + 8], eax
    mov dword [boot_pdpt + 12], 0
    mov eax, boot_pd + 8192
    or eax, 0x03
    mov dword [boot_pdpt + 16], eax
    mov dword [boot_pdpt + 20], 0
    mov eax, boot_pd + 12288
    or eax, 0x03
    mov dword [boot_pdpt + 24], eax
    mov dword [boot_pdpt + 28], 0

    ; Identity map 0..4 GiB with 2 MiB pages.  The final kernel paging code
    ; replaces this temporary map before entering user space.
    mov edi, boot_pd
    xor ebx, ebx
    mov ecx, 2048
.map_2m:
    mov eax, ebx
    or eax, 0x83                 ; present | writable | 2 MiB page
    mov dword [edi], eax
    mov dword [edi + 4], 0
    add ebx, 0x200000
    add edi, 8
    loop .map_2m

    ; Load the long-mode GDT while its descriptor still uses the 32-bit form.
    lgdt [boot_gdt_ptr]

    ; PAE is required before setting EFER.LME.  x86_64 C compilers may emit
    ; SSE2 even before fpu_init(), so make XMM instructions legal before the
    ; first C instruction (OSFXSR | OSXMMEXCPT).
    mov eax, cr4
    or eax, (1 << 5) | (1 << 9) | (1 << 10)
    mov cr4, eax
    mov eax, boot_pml4
    mov cr3, eax

    mov ecx, 0xC0000080          ; IA32_EFER
    rdmsr
    or eax, (1 << 8) | (1 << 11) ; LME | NXE
    wrmsr

    mov eax, cr0
    and eax, ~((1 << 2) | (1 << 3)) ; clear EM and TS
    or eax, (1 << 1) | (1 << 31) ; MP | paging; PE is already set
    mov cr0, eax
    jmp 0x08:long_mode_start

bits 64
long_mode_start:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    xor eax, eax
    mov fs, ax
    mov gs, ax
    mov rsp, __boot_stack_top
    and rsp, -16
    mov edi, ebp
    xor rbp, rbp

    ; SysV x86_64: RDI=magic, RSI=multiboot info.  The 32-bit writes above
    ; zero-extended both registers.
    call kernel_main
.hang:
    cli
    hlt
    jmp .hang

section .rodata
align 8
boot_gdt:
    dq 0
    dq 0x00AF9A000000FFFF       ; 0x08: 64-bit kernel code
    dq 0x00CF92000000FFFF       ; 0x10: kernel data
boot_gdt_end:
boot_gdt_ptr:
    dw boot_gdt_end - boot_gdt - 1
    dd boot_gdt

; Kept outside .bss: kernel_main clears .bss after paging is already active.
section .boot_paging nobits alloc noexec write align=4096
align 4096
boot_pml4: resb 4096
boot_pdpt: resb 4096
boot_pd:   resb 4 * 4096
