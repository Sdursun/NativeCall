/*
    QEMU stand-in for the Pi firmware + boot.S (qemu/README.md). ./run.sh stub runs the real armstub
    instead; this one also works on raspi3b, which has no peripherals where boot.S expects them.
    QEMU loads this as a Linux arm64 Image at 0x80000, DTB in x0, secondaries in its own spin stub.

    hv_fdt_reserve_top() takes RESERVE bytes off the top of the last /memory block. If it agrees, the
    hypervisor is copied there and entered; if it refuses, Emu68 starts with no hypervisor, as with the
    stock armstub. The status is kept at fdt_status for the debugger.
*/

#include "hv_config.h"

#define EMU68_LOAD      0x00280000

        .section .text.boot, "ax"
        .globl  _start
_start:
        b       entry                   /* code0 */
        .long   0                       /* code1 */
        .quad   0x80000                 /* text_offset */
        .quad   0x100000                /* image_size */
        .quad   0x2                     /* flags: LE, 4K pages, 2 MB aligned base */
        .quad   0, 0, 0                 /* res2-res4 */
        .ascii  "ARM\x64"               /* magic */
        .long   0                       /* res5 */

entry:
        mov     x19, x0                 /* DTB */

        mrs     x9, CurrentEL
        cmp     x9, #8
        b.eq    at_el2
        b.lt    stuck                   /* EL1: no way to install an EL2 handler */

        mov     x9, #((1 << 10) | (1 << 8) | (1 << 7) | (1 << 5) | (1 << 4) | 1)
        msr     SCR_EL3, x9
        msr     CPTR_EL3, xzr
        ldr     x9, =0x30c50830
        msr     SCTLR_EL2, x9
        mov     x9, #0x3c9
        msr     SPSR_EL3, x9
        adr     x9, at_el2
        msr     ELR_EL3, x9
        eret

at_el2:
        mrs     x9, MIDR_EL1
        msr     VPIDR_EL2, x9
        mrs     x9, MPIDR_EL1
        msr     VMPIDR_EL2, x9

        /* Below the image, unused at this point (boot.S would use the same kind of scratch) */
        mov     x9, #0x70000
        mov     sp, x9
        sub     sp, sp, #32

        mov     x0, x19
        ldr     x1, =RESERVE
        mov     x2, sp
        bl      hv_fdt_reserve_top
        adr     x9, fdt_status
        str     x0, [x9]
        cbnz    x0, no_hypervisor

        ldr     x20, [sp]               /* result.base */
        ldr     w21, [sp, #8]           /* result.gb_mask */
        adr     x9, hv_base
        str     x20, [x9]

        adr     x5, hv_blob_start
        adr     x7, hv_blob_end
        mov     x6, x20
1:      ldp     x8, x9, [x5], #16
        stp     x8, x9, [x6], #16
        cmp     x5, x7
        b.lo    1b

        dsb     sy
        ic      iallu
        dsb     sy
        isb

        /* Send the secondaries through our loop, which continues in hv_secondary_spin */
        adr     x9, secondary
        mov     x10, #0xe0
        str     x9, [x10]
        str     x9, [x10, #8]
        str     x9, [x10, #16]
        dsb     sy
        sev

        mov     x0, x19
        ldr     x1, =EMU68_LOAD
        mov     x3, x21
        br      x20

        /* Refused: start Emu68 at EL2 as the stock armstub does; QEMU's stub keeps the secondaries */
no_hypervisor:
        mov     x0, x19
        mov     x1, xzr
        mov     x2, xzr
        mov     x3, xzr
        ldr     x4, =EMU68_LOAD
        br      x4

secondary:
        mrs     x9, CurrentEL
        cmp     x9, #8
        b.eq    sec_el2
        b.lt    stuck
        mov     x9, #((1 << 10) | (1 << 8) | (1 << 7) | (1 << 5) | (1 << 4) | 1)
        msr     SCR_EL3, x9
        msr     CPTR_EL3, xzr
        ldr     x9, =0x30c50830
        msr     SCTLR_EL2, x9
        mov     x9, #0x3c9
        msr     SPSR_EL3, x9
        adr     x9, sec_el2
        msr     ELR_EL3, x9
        eret
sec_el2:
        mrs     x9, MIDR_EL1
        msr     VPIDR_EL2, x9
        mrs     x9, MPIDR_EL1
        msr     VMPIDR_EL2, x9
        ldr     x6, hv_base
        add     x6, x6, #8
        br      x6

stuck:  wfe
        b       stuck

        .ltorg
        .balign 8
hv_base:        .quad 0
fdt_status:     .quad 0

        .balign 16
hv_blob_start:
        .incbin "hv.bin"
        .balign 16
hv_blob_end:
