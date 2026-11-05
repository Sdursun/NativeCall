/*
    Native module layout: one flat, position independent binary (objcopy -O binary).
    The module is linked at NARM_BASE (0 for the real build; the Makefile links a second copy
    at another base and compares them to prove position independence).
*/
OUTPUT_FORMAT("elf64-bigaarch64")
ENTRY(__narm_start)     /* no single entry point: the header lists them */

SECTIONS
{
    . = NARM_BASE;
    __narm_start = .;

    /* struct NativeHeader (nc_abi.h). Entries hold absolute addresses = offsets when NARM_BASE = 0 */
    .narm_header :
    {
        LONG(0x4E41524D)                                    /* 'NARM' */
        LONG(1)                                             /* NARM_VERSION */
        LONG((__narm_entries_end - __narm_entries) / 4)     /* entry_count */
        __narm_entries = .;
        KEEP(*(.narm_entries))
        __narm_entries_end = .;
    }

    . = ALIGN(16);
    .text : { *(.text .text.*) }
    .rodata : { *(.rodata .rodata.*) }

    /* .bss goes into the file as zeros: the 68k loader only allocates the file size */
    .data : { *(.data .data.*) *(.bss .bss.*) *(COMMON) }
    . = ALIGN(16);
    __narm_end = .;

    .got : { *(.got .got.*) }
    .got.plt : { *(.got.plt) }
    .rela.dyn : { *(.rela*) }

    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.interp) *(.dynamic) *(.dynsym) *(.dynstr) *(.hash) *(.gnu.hash) }
}

ASSERT(SIZEOF(.got) == 0 && SIZEOF(.got.plt) == 0, "native module uses a GOT: build with -fPIE -fvisibility=hidden")
ASSERT(SIZEOF(.rela.dyn) == 0, "native module needs dynamic relocations")
ASSERT(__narm_entries_end > __narm_entries, "native module exports nothing: NC_MODULE() missing in the kernels file")
