ENTRY(_start)

SECTIONS
{
    . = 0x100000;
    __kernel_start = .;

    .text : {
        . = ALIGN(8);
        *(.multiboot)
        *(.text.entry)
        *(.text*)
    }

    .rodata : {
        *(.rodata*)
    }

    .data : {
        *(.data*)
    }

    __bss_start = .;
    .bss : {
        *(COMMON)
        *(.bss*)
    }
    __bss_end = .;

    /* The bootstrap/idle stack must follow the variable-sized kernel image.
       A fixed physical address eventually collides with growing .rodata/.bss.
       Keep it outside the range cleared by zero_bss(), but inside the range
       reserved from the physical allocator. */
    .boot_stack (NOLOAD) : ALIGN(16) {
        __boot_stack_bottom = .;
        . += 0x10000;
        __boot_stack_top = .;
    }
    __kernel_end = .;
}
