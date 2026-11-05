/* Resident hypervisor, linked at HV_BASE. Preprocessed with cpp (see Makefile). */
#include "hv_config.h"

OUTPUT_FORMAT("elf64-bigaarch64")
ENTRY(hv_entry)

SECTIONS
{
    . = HV_BASE;

    .text : { *(.text.entry) *(.text .text.*) }
    .rodata : { *(.rodata .rodata.*) }
    .data : { *(.data .data.*) }
    . = ALIGN(16);

    /* Not part of hv.bin: zeroed by hv_entry */
    .bss (NOLOAD) : ALIGN(4096)
    {
        __bss_start = .;
        *(.bss .bss.* COMMON)
        . = ALIGN(16);
        . += HV_STACK_SIZE;
        __stack_top = .;
        __bss_end = .;
    }

    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.interp) *(.dynamic) }
}
