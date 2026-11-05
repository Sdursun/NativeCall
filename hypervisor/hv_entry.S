/*
    Resident EL2 part of the nativecall armstub. Linked at HV_BASE, big-endian data
    (instructions are always little-endian on AArch64).

    HV_BASE + 0     hv_entry: once from boot.S on core 0 at EL2, x0 = DTB, x1 = kernel entry
    HV_BASE + 4     hv_secondary: cores 1-3 at EL2 when Emu68 starts them, x0 = their entry
    HV_BASE + 8     hv_secondary_spin: the spin-table loop itself (the QEMU test has no
                    boot.S for the secondary cores)
    hv_vectors      VBAR_EL2 of core 0 and of the helper cores
    nc_call_native  runs a native function with all FP/SIMD state saved, recovers from faults
                    (per core: TPIDR_EL2 = struct hv_cpu)
*/

#include "hv_config.h"

        .section .text.entry, "ax"
        b       hv_entry
        b       hv_secondary
        b       hv_secondary_spin

        .globl  hv_entry
hv_entry:
        /* Big-endian data from here on. No data loads before this point. */
        mrs     x2, SCTLR_EL2
        orr     x2, x2, #SCTLR_EL2_EE
        msr     SCTLR_EL2, x2
        isb

        mov     x19, x0                 /* DTB */
        mov     x20, x1                 /* kernel entry */

        ldr     x2, =__stack_top
        mov     sp, x2

        ldr     x2, =__bss_start
        ldr     x3, =__bss_end
1:      cmp     x2, x3
        b.hs    2f
        str     xzr, [x2], #8
        b       1b
2:
        ldr     x2, =hv_cpu             /* core 0's state */
        msr     TPIDR_EL2, x2

        bl      hv_init                 /* page tables, EL2 MMU + caches */

        ldr     x2, =hv_vectors
        msr     VBAR_EL2, x2

        /* What Emu68's leave_EL2 would do (src/aarch64/start.c:184-201), plus our trap */
        mrs     x2, MDCR_EL2
        orr     x2, x2, #0x80           /* HPME: event counters */
        msr     MDCR_EL2, x2
        mov     x2, #3                  /* EL1PCTEN | EL1PCEN */
        msr     CNTHCTL_EL2, x2
        msr     CNTVOFF_EL2, xzr
        ldr     x2, =CPTR_EL2_INIT
        msr     CPTR_EL2, x2
        msr     VTTBR_EL2, xzr
        ldr     x2, =(HCR_RW | HCR_TPC)
        msr     HCR_EL2, x2

        ldr     x2, =SCTLR_EL1_INIT
        msr     SCTLR_EL1, x2

        bl      hv_led_off

        mov     x2, #SPSR_EL1H_DAIF
        msr     SPSR_EL2, x2
        msr     ELR_EL2, x20
        mov     x0, x19
        mov     x1, xzr
        mov     x2, xzr
        mov     x3, xzr
        isb
        eret                            /* Emu68 _start at EL1 -> continue_boot */

        .ltorg

/* ---- Secondary cores --------------------------------------------------------------------------

   hv_secondary_spin: the firmware's spin-table loop (0xd8 + 8 * core, written by Emu68 with
   LE64), used by the QEMU test. Little-endian data, MMU off, no memory of ours touched. */
hv_secondary_spin:
        mrs     x6, MPIDR_EL1
        and     x6, x6, #3
        mov     x5, #0xd8
        str     xzr, [x5, x6, lsl #3]   /* the loader parked our own address here */
        dsb     sy
1:      wfe
        ldr     x0, [x5, x6, lsl #3]
        cbz     x0, 1b
        /* fall through */

/* hv_secondary: x0 = Emu68's _secondary_start (physical). EL2, MMU off, little-endian data.
   Core 2 and anything unexpected go straight to Emu68, exactly as without the hypervisor.
   Cores 1 and 3: EL2 MMU (same tables as core 0), stack, vectors, MDCR_EL2.TPM, then Emu68's
   own EL2 code, which drops to EL1. Nothing may be read from memory before the MMU is on:
   with it off data accesses bypass the caches and see stale RAM. */
hv_secondary:
        mrs     x6, MPIDR_EL1
        and     x6, x6, #3
        mov     x7, #HV_HELPER_MASK
        lsr     x7, x7, x6
        tbz     x7, #0, hv_to_emu68
        /* only RAM our EL2 map has (hv.c): 16 MB .. 0xFC000000 and 4 .. 8 GB */
        mov     x7, #0x01000000
        cmp     x0, x7
        b.lo    hv_to_emu68
        mov     x7, #0xfc000000
        cmp     x0, x7
        b.lo    1f
        mov     x7, #0x100000000
        cmp     x0, x7
        b.lo    hv_to_emu68
        mov     x7, #0x200000000
        cmp     x0, x7
        b.hs    hv_to_emu68
1:
        mov     x19, x0
        mov     x20, x6

        mrs     x2, SCTLR_EL2           /* big-endian data, then the literal pool is usable */
        orr     x2, x2, #SCTLR_EL2_EE
        msr     SCTLR_EL2, x2
        isb

        ldr     x2, =MAIR_EL2_VAL
        msr     MAIR_EL2, x2
        ldr     x2, =TCR_EL2_VAL
        msr     TCR_EL2, x2
        ldr     x2, =hv_l1
        msr     TTBR0_EL2, x2
        isb
        tlbi    alle2
        dsb     sy
        isb
        mrs     x2, SCTLR_EL2
        mov     x3, #SCTLR_EL2_MCI
        orr     x2, x2, x3
        bic     x2, x2, #SCTLR_EL2_A
        bic     x2, x2, #SCTLR_EL2_WXN
        msr     SCTLR_EL2, x2
        isb
        ic      iallu
        dsb     sy
        isb

        ldr     x2, =hv_cpu
        mov     x3, #CPU_SIZE
        madd    x2, x20, x3, x2
        msr     TPIDR_EL2, x2
        ldr     x2, =hv_stacks
        add     x3, x20, #1
        lsl     x3, x3, #HV_STACK_SHIFT
        add     x2, x2, x3
        mov     sp, x2
        ldr     x2, =hv_vectors
        msr     VBAR_EL2, x2
        ldr     x2, =CPTR_EL2_INIT
        msr     CPTR_EL2, x2
        mrs     x2, MDCR_EL2
        orr     x2, x2, #MDCR_TPM
        msr     MDCR_EL2, x2
        isb
        mov     x0, x19

hv_to_emu68:
        mov     x4, x0
        mov     x0, xzr
        mov     x1, xzr
        mov     x2, xzr
        mov     x3, xzr
        br      x4

        .ltorg

/* ---- Exception vectors ------------------------------------------------------------------------ */

        .macro  ventry target
        .balign 0x80
        b       \target
        .endm

        .text
        .balign 0x800
        .globl  hv_vectors
hv_vectors:
        ventry  hv_panic                /* current EL, SP_EL0: sync */
        ventry  hv_panic                /*                     irq  */
        ventry  hv_panic                /*                     fiq  */
        ventry  hv_panic                /*                     serr */
        ventry  hv_native_fault         /* current EL, SP_ELx: sync (native code crashed) */
        ventry  hv_panic
        ventry  hv_panic
        ventry  hv_panic
        ventry  hv_lower_sync           /* lower EL, AArch64: sync (the DC trap) */
        ventry  hv_panic
        ventry  hv_panic
        ventry  hv_panic
        ventry  hv_panic                /* lower EL, AArch32 */
        ventry  hv_panic
        ventry  hv_panic
        ventry  hv_panic

/* Emu68 (EL1) executed a trapped instruction. Save x0-x30, let hv_sync() handle it.
   SPSR_EL2 goes into slot 31 of the frame: a fault in native code is a second exception at
   EL2 and overwrites it (ELR_EL2 is rewritten by hv_sync itself). Without this the eret
   below would stay at EL2. */
hv_lower_sync:
        sub     sp, sp, #(32 * 8)
        stp     x0,  x1,  [sp, #(0 * 8)]
        stp     x2,  x3,  [sp, #(2 * 8)]
        stp     x4,  x5,  [sp, #(4 * 8)]
        stp     x6,  x7,  [sp, #(6 * 8)]
        stp     x8,  x9,  [sp, #(8 * 8)]
        stp     x10, x11, [sp, #(10 * 8)]
        stp     x12, x13, [sp, #(12 * 8)]
        stp     x14, x15, [sp, #(14 * 8)]
        stp     x16, x17, [sp, #(16 * 8)]
        stp     x18, x19, [sp, #(18 * 8)]
        stp     x20, x21, [sp, #(20 * 8)]
        stp     x22, x23, [sp, #(22 * 8)]
        stp     x24, x25, [sp, #(24 * 8)]
        stp     x26, x27, [sp, #(26 * 8)]
        stp     x28, x29, [sp, #(28 * 8)]
        mrs     x0, SPSR_EL2
        stp     x30, x0,  [sp, #(30 * 8)]

        mov     x0, sp
        bl      hv_sync

        ldr     x0, [sp, #(31 * 8)]
        msr     SPSR_EL2, x0
        ldp     x0,  x1,  [sp, #(0 * 8)]
        ldp     x2,  x3,  [sp, #(2 * 8)]
        ldp     x4,  x5,  [sp, #(4 * 8)]
        ldp     x6,  x7,  [sp, #(6 * 8)]
        ldp     x8,  x9,  [sp, #(8 * 8)]
        ldp     x10, x11, [sp, #(10 * 8)]
        ldp     x12, x13, [sp, #(12 * 8)]
        ldp     x14, x15, [sp, #(14 * 8)]
        ldp     x16, x17, [sp, #(16 * 8)]
        ldp     x18, x19, [sp, #(18 * 8)]
        ldp     x20, x21, [sp, #(20 * 8)]
        ldp     x22, x23, [sp, #(22 * 8)]
        ldp     x24, x25, [sp, #(24 * 8)]
        ldp     x26, x27, [sp, #(26 * 8)]
        ldp     x28, x29, [sp, #(28 * 8)]
        ldr     x30,      [sp, #(30 * 8)]
        add     sp, sp, #(32 * 8)
        eret

/* Anything unexpected: stop this core. Emu68 hangs the same way on unhandled exceptions. */
        .globl  hv_panic
hv_panic:
        mrs     x0, ESR_EL2
        mrs     x1, ELR_EL2
        mrs     x2, FAR_EL2
        ldr     x3, =hv_panic_info
        stp     x0, x1, [x3]
        str     x2, [x3, #16]
1:      wfe
        b       1b

/* ---- Native call ------------------------------------------------------------------------------

   uint32_t nc_call_native(uintptr_t fn, uintptr_t params)

   The caller (hv.c) is built with -mgeneral-regs-only, so Emu68's FP/SIMD state (v8-v15 =
   FP0-FP7, v19-v21 = SR/FPSR/context, ...) is still live here. Save all of it, run the native
   code with a default FPCR, restore.

   If the native code takes an exception, hv_native_fault returns here with w0 = 0xffffffff
   and hv_fault_flag = 1. */

#define FRAME_FPCR  0
#define FRAME_Q     16
#define FRAME_SIZE  (16 + 32 * 16)

        .globl  nc_call_native
nc_call_native:
        stp     x29, x30, [sp, #-16]!
        mov     x29, sp
        sub     sp, sp, #FRAME_SIZE

        stp     q0,  q1,  [sp, #(FRAME_Q + 0 * 16)]
        stp     q2,  q3,  [sp, #(FRAME_Q + 2 * 16)]
        stp     q4,  q5,  [sp, #(FRAME_Q + 4 * 16)]
        stp     q6,  q7,  [sp, #(FRAME_Q + 6 * 16)]
        stp     q8,  q9,  [sp, #(FRAME_Q + 8 * 16)]
        stp     q10, q11, [sp, #(FRAME_Q + 10 * 16)]
        stp     q12, q13, [sp, #(FRAME_Q + 12 * 16)]
        stp     q14, q15, [sp, #(FRAME_Q + 14 * 16)]
        stp     q16, q17, [sp, #(FRAME_Q + 16 * 16)]
        stp     q18, q19, [sp, #(FRAME_Q + 18 * 16)]
        stp     q20, q21, [sp, #(FRAME_Q + 20 * 16)]
        stp     q22, q23, [sp, #(FRAME_Q + 22 * 16)]
        stp     q24, q25, [sp, #(FRAME_Q + 24 * 16)]
        stp     q26, q27, [sp, #(FRAME_Q + 26 * 16)]
        stp     q28, q29, [sp, #(FRAME_Q + 28 * 16)]
        stp     q30, q31, [sp, #(FRAME_Q + 30 * 16)]
        mrs     x2, FPCR
        mrs     x3, FPSR
        stp     x2, x3, [sp, #FRAME_FPCR]

        /* Recovery point for hv_native_fault: callee-saved registers and sp (this core's) */
        mrs     x2, TPIDR_EL2
        add     x2, x2, #CPU_JMPBUF
        stp     x19, x20, [x2, #(0 * 8)]
        stp     x21, x22, [x2, #(2 * 8)]
        stp     x23, x24, [x2, #(4 * 8)]
        stp     x25, x26, [x2, #(6 * 8)]
        stp     x27, x28, [x2, #(8 * 8)]
        mov     x3, sp
        stp     x29, x3,  [x2, #(10 * 8)]

        mrs     x2, TPIDR_EL2
        mov     w3, #1
        str     w3, [x2, #CPU_IN_NATIVE]

        msr     FPCR, xzr
        msr     FPSR, xzr
        mov     x16, x0
        mov     x0, x1
        blr     x16

nc_native_return:
        mrs     x2, TPIDR_EL2
        str     wzr, [x2, #CPU_IN_NATIVE]

        ldp     x2, x3, [sp, #FRAME_FPCR]
        msr     FPCR, x2
        msr     FPSR, x3
        ldp     q0,  q1,  [sp, #(FRAME_Q + 0 * 16)]
        ldp     q2,  q3,  [sp, #(FRAME_Q + 2 * 16)]
        ldp     q4,  q5,  [sp, #(FRAME_Q + 4 * 16)]
        ldp     q6,  q7,  [sp, #(FRAME_Q + 6 * 16)]
        ldp     q8,  q9,  [sp, #(FRAME_Q + 8 * 16)]
        ldp     q10, q11, [sp, #(FRAME_Q + 10 * 16)]
        ldp     q12, q13, [sp, #(FRAME_Q + 12 * 16)]
        ldp     q14, q15, [sp, #(FRAME_Q + 14 * 16)]
        ldp     q16, q17, [sp, #(FRAME_Q + 16 * 16)]
        ldp     q18, q19, [sp, #(FRAME_Q + 18 * 16)]
        ldp     q20, q21, [sp, #(FRAME_Q + 20 * 16)]
        ldp     q22, q23, [sp, #(FRAME_Q + 22 * 16)]
        ldp     q24, q25, [sp, #(FRAME_Q + 24 * 16)]
        ldp     q26, q27, [sp, #(FRAME_Q + 26 * 16)]
        ldp     q28, q29, [sp, #(FRAME_Q + 28 * 16)]
        ldp     q30, q31, [sp, #(FRAME_Q + 30 * 16)]

        add     sp, sp, #FRAME_SIZE
        ldp     x29, x30, [sp], #16
        ret

/* Synchronous exception at EL2. Only recoverable while native code runs. */
hv_native_fault:
        mrs     x5, TPIDR_EL2
        ldr     w3, [x5, #CPU_IN_NATIVE]
        cbz     w3, hv_panic
        str     wzr, [x5, #CPU_IN_NATIVE]

        mrs     x0, ESR_EL2
        mrs     x1, FAR_EL2
        mrs     x4, ELR_EL2
        add     x2, x5, #CPU_FAULT_INFO
        stp     x0, x1, [x2]
        str     x4, [x2, #16]
        mov     w3, #1
        str     w3, [x5, #CPU_FAULT_FLAG]

        add     x2, x5, #CPU_JMPBUF
        ldp     x19, x20, [x2, #(0 * 8)]
        ldp     x21, x22, [x2, #(2 * 8)]
        ldp     x23, x24, [x2, #(4 * 8)]
        ldp     x25, x26, [x2, #(6 * 8)]
        ldp     x27, x28, [x2, #(8 * 8)]
        ldp     x29, x3,  [x2, #(10 * 8)]
        mov     sp, x3
        mov     w0, #0xffffffff
        b       nc_native_return

        .ltorg

        .bss
        .balign 8
        .globl  hv_panic_info
hv_panic_info:  .space  3 * 8           /* ESR, ELR, FAR of a panic */
