/*
    QEMU stand-in for the Pi firmware + armstub (qemu/README.md).

    QEMU's raspi3b loads this as a Linux arm64 Image at 0x80000, passes the DTB in x0 and keeps
    the secondary cores in its own spin-table stub (0xd8..0xf0), like the Pi firmware. Emu68
    is loaded by "-device loader" at EMU68_LOAD. We do what boot.S does for core 0 on the Pi:
    copy the hypervisor to HV_BASE and enter hv_entry at EL2 with x0 = DTB, x1 = Emu68 entry.
    Little-endian, like boot.S.
*/

#define EMU68_LOAD      0x00280000

        .section .text
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

        /* EL3 -> EL2, as boot.S */
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

        adr     x5, hv_blob_start
        adr     x7, hv_blob_end
        ldr     x6, =HV_BASE
1:      ldp     x8, x9, [x5], #16
        stp     x8, x9, [x6], #16
        cmp     x5, x7
        b.lo    1b

        dsb     sy
        ic      iallu
        dsb     sy
        isb

        /* Secondary cores: on the Pi they spin in boot.S and then go through hv_secondary.
           Here QEMU's stub holds them; send them to our own loop (secondary below), which
           continues in hv_secondary_spin (HV_BASE + 8) on the same spin-table words. */
        adr     x9, secondary
        mov     x10, #0xe0
        str     x9, [x10]
        str     x9, [x10, #8]
        str     x9, [x10, #16]
        dsb     sy
        sev

        mov     x0, x19
        ldr     x1, =EMU68_LOAD
        ldr     x6, =HV_BASE
        br      x6

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
        ldr     x6, =HV_BASE
        add     x6, x6, #8
        br      x6

stuck:  wfe
        b       stuck

        .ltorg

        .balign 16
hv_blob_start:
        .incbin "hv.bin"
        .balign 16
hv_blob_end:
