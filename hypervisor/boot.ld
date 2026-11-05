/* armstub: loaded by the firmware at physical address 0 */
OUTPUT_FORMAT("elf64-littleaarch64")
ENTRY(_start)

SECTIONS
{
    . = 0;
    .text : { *(.text) }
    /DISCARD/ : { *(.comment) *(.note*) *(.data) *(.bss) }
}
