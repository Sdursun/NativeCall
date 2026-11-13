/* Shared by boot.S, hv_entry.S, hv.c and hv.ld (via cpp). Plain #defines only. */
#ifndef HV_CONFIG_H
#define HV_CONFIG_H

/*
    The resident hypervisor is position independent and has no fixed address. boot.S takes HV_RESERVE
    bytes off the top of the last /memory block of the device tree (hv_fdt.c) and copies it there:
    Emu68 takes its own memory from the top of that same block, and never sees what is above the
    new top (NOTES.md 3, 25). A multiple of 2 MB, because Emu68 needs its top of RAM aligned so.
*/
#define HV_RESERVE          0x200000

#define HV_STACK_SIZE       0x10000         /* EL2 stack of each core, native code runs on it */
#define HV_STACK_SHIFT      16

/*
    Helper cores for NC_OP_RUNMP (NOTES.md 22): Emu68 leaves cores 1 and 3 idle in a "wfe" loop
    (core 2 is the PiStorm housekeeper, never touched). They enter Emu68 through hv_secondary
    with MDCR_EL2.TPM set; Emu68 keeps that bit (read-modify-write) and its secondary_boot()
    reads PMCR_EL0 first thing at EL1, which traps here. Then HCR_EL2.TWE is switched on: every
    idle "wfe" of the core comes to the hypervisor, which runs pieces of RUNMP requests.
*/
#define HV_HELPER_MASK      0xa             /* cores 1 and 3 */
/* Cores hv_secondary sets up at EL2 (vectors, MMU, stack, the PMU trap). With HV_S2 also core 2,
   the PiStorm housekeeper: only for stage 2, it never becomes a helper (hv.c, helper_sync). */
#if defined(HV_S2) || defined(HV_CAPTURE_CORE2)
#define HV_CAPTURE_CORE2    1
#define HV_CAPTURE_MASK     0xe
#else
#define HV_CAPTURE_MASK     HV_HELPER_MASK
#endif

/* Per core state (struct hv_cpu in hv.c), TPIDR_EL2 points at it */
#define CPU_JMPBUF          0               /* 12 * 8 */
#define CPU_IN_NATIVE       96
#define CPU_FAULT_FLAG      100
#define CPU_FAULT_INFO      104             /* 3 * 8: ESR, FAR, ELR */
#define CPU_REGS            256             /* 32 * 8: x0-x30, sp of the native code at its fault */
#define CPU_SIZE            768

/* EL2 MMU (hv.c builds the tables, hv_entry.S switches it on for the helper cores) */
#define MAIR_EL2_VAL        0x04ff          /* attr0 Normal WB, attr1 Device-nGnRE */
/* RES1 31, 23 | PS 36 bit | SH0 inner | ORGN0/IRGN0 WB-WA | TG0 4K | T0SZ 30 */
#define TCR_EL2_VAL         0x8081351e
#define SCTLR_EL2_MCI       0x1005          /* M, C, I */
#define SCTLR_EL2_A         (1 << 1)
#define SCTLR_EL2_WXN       (1 << 19)

/* HCR_EL2 for core 0 while Emu68 runs */
#define HCR_RW              (1 << 31)       /* EL1 is AArch64 */
#define HCR_TPC             (1 << 23)       /* trap DC CIVAC/CVAC/IVAC from EL1 */
#define HCR_TWE             (1 << 14)       /* helper cores: trap WFE from EL1 */
#define HCR_FMO             (1 << 3)        /* FIQs go to EL2: the time limit (below) */
#define HCR_VM              (1 << 0)        /* stage 2 for EL1 (HV_S2: the hypervisor hidden from Emu68) */
/* VTCR_EL2: RES1 31 | PS 36 bit | SH0 inner | ORGN0/IRGN0 WB-WA | TG0 4K | SL0 level 1 | T0SZ 28 */
#define VTCR_EL2_VAL        0x8001355c

/*
    Time limit of native code (hypervisor 6): the EL2 physical timer (CNTHP, PPI 26) is armed
    only while native code runs, with PSTATE.F clear. boot.S makes PPI 26 the only Group 0
    interrupt of each core and lets the GIC signal Group 0 as FIQ; HCR_EL2.FMO takes FIQs to
    EL2. Emu68 uses only Group 1 IRQs (core 0: Amiga level 6, core 3: CNTP for the PPC) and
    never touches CNTHP. A FIQ in native code ends the call like a fault (fault_flag 2).
*/
#define HV_TIMER_PPI        26              /* CNTHP of each core in the GIC */
#define FAULT_CRASH         1
#define FAULT_TIMEOUT       2

#define MDCR_TPM            (1 << 6)        /* helper cores: trap PMU access from EL1 (once) */
/* helper cores: event stream from counter bit 13 (an event every ~300 us), wakes their "wfe" */
#define CNTHCTL_EVNT        ((1 << 2) | (13 << 4))

#define SCTLR_EL1_INIT      0x30d00800      /* RES1 bits only: MMU/caches off, LE */
#define SCTLR_EL2_EE        (1 << 25)
#define SPSR_EL1H_DAIF      0x3c5           /* EL1h, D/A/I/F masked (as Emu68's leave_EL2) */
#define CPTR_EL2_INIT       0x33ff          /* RES1 bits, TFP=0: no FP/SIMD trapping */

/*
    Boot progress on the green ACT LED (GPIO 42 on the Pi 4), build with DEBUG_LED=1:
      1 long flash (1 s)   boot.S runs on core 0, about to read the device tree
      2 short flashes      hv_init runs from its new place (copy worked, RAM there is usable)
      3 short flashes      EL2 MMU and caches are on
      LED off              entering Emu68 at EL1
    Device tree refused: a second long flash, then one short flash per HV_FDT_* status (hv_fdt.h),
    and Emu68 starts without the hypervisor.
    GPIO registers are little-endian: the big-endian hypervisor swaps.
*/
#define GPIO_BASE           0xfe200000
#define GPFSEL4             0x10
#define GPSET1              0x20
#define GPCLR1              0x2c
#define LED_GPIO_BIT        (1 << (42 - 32))
#define LED_FSEL_SHIFT      ((42 - 40) * 3)
#define CNT_FREQ            54000000

#endif
