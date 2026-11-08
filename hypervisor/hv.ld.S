/* Resident hypervisor, position independent. The Makefile links it at 0 and at HV_LINK = 2 MB and
   compares: no absolute address may depend on the base. Preprocessed with cpp. */
#include "hv_config.h"

OUTPUT_FORMAT("elf64-bigaarch64")
ENTRY(hv_entry)

SECTIONS
{
    . = HV_LINK;
    __hv_start = .;

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

    .got : { *(.got .got.*) }
    .rela.dyn : { *(.rela*) }

    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.interp) *(.dynamic) }
}

ASSERT(SIZEOF(.got) == 0, "hypervisor uses a GOT: it must be position independent without one")
ASSERT(SIZEOF(.rela.dyn) == 0, "hypervisor needs relocations")
ASSERT(__bss_end - __hv_start <= HV_RESERVE, "hypervisor does not fit in HV_RESERVE")
