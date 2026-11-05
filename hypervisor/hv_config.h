/* Shared by boot.S, hv_entry.S, hv.c and hv.ld (via cpp). Plain #defines only. */
#ifndef HV_CONFIG_H
#define HV_CONFIG_H

/*
    Physical (= EL2 virtual) address of the resident hypervisor. On a Pi 4 with 8 GB this is
    the start of RAM above 4 GB. Emu68 never hands it to the 68k (0xF2000000 limit) and takes
    its own memory (kernel, JIT, MMU pool) from the top of the last block. See NOTES.md 3, 9.2.
    Must be a multiple of 4 GB: boot.S loads it with a single movz.
*/
#ifndef HV_BASE
#define HV_BASE             0x100000000     /* the QEMU test (../qemu) overrides it */
#endif

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

/* Per core state (struct hv_cpu in hv.c), TPIDR_EL2 points at it */
#define CPU_JMPBUF          0               /* 12 * 8 */
#define CPU_IN_NATIVE       96
#define CPU_FAULT_FLAG      100
#define CPU_FAULT_INFO      104             /* 3 * 8: ESR, FAR, ELR */
#define CPU_SIZE            512

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
#define MDCR_TPM            (1 << 6)        /* helper cores: trap PMU access from EL1 (once) */
/* helper cores: event stream from counter bit 13 (an event every ~300 us), wakes their "wfe" */
#define CNTHCTL_EVNT        ((1 << 2) | (13 << 4))

#define SCTLR_EL1_INIT      0x30d00800      /* RES1 bits only: MMU/caches off, LE */
#define SCTLR_EL2_EE        (1 << 25)
#define SPSR_EL1H_DAIF      0x3c5           /* EL1h, D/A/I/F masked (as Emu68's leave_EL2) */
#define CPTR_EL2_INIT       0x33ff          /* RES1 bits, TFP=0: no FP/SIMD trapping */

/*
    Boot progress on the green ACT LED (GPIO 42 on the Pi 4), build with DEBUG_LED=1:
      1 long flash (1 s)   boot.S runs on core 0, about to copy the hypervisor above 4 GB
      2 short flashes      hv_init runs from above 4 GB (copy worked, RAM there is usable)
      3 short flashes      EL2 MMU and caches are on
      LED off              entering Emu68 at EL1
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
