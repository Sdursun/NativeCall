/* Resident hypervisor, position independent. The Makefile links it at 0 and at HV_LINK = 2 MB and
   compares: no absolute address may depend on the base. Preprocessed with cpp.

   Page-aligned parts, for the 4 KB map of the hypervisor itself (hv.c, l3_hv):
     [__hv_start, __ro_end)     code and read-only data: read-only
     [__pt_start, __pt_end)     page tables: read-only once the MMU is on
     the rest of .bss           read-write, except the lowest page of every stack (guard) */
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
    __image_end = .;
    . = ALIGN(4096);
    __ro_end = .;

    /* Not part of hv.bin: zeroed by hv_entry */
    .bss (NOLOAD) : ALIGN(4096)
    {
        __bss_start = .;
        __pt_start = .;
        *(.bss.pt)
        . = ALIGN(4096);
        __pt_end = .;
        *(.bss .bss.* COMMON)
        . = ALIGN(4096);
        __stack0_bottom = .;
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
ASSERT(SIZEOF(.data) == 0, "hypervisor has writable .data: it would be mapped read-only (hv.c, l3_hv)")
ASSERT(__bss_end - __hv_start <= HV_RESERVE, "hypervisor does not fit in HV_RESERVE")
