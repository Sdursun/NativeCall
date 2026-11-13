/*
    Resident EL2 part of the nativecall armstub (C side).

    Built with -mgeneral-regs-only: while a trap is handled, Emu68's FP/SIMD registers
    hold live 68k state and must not be touched. nc_call_native() saves them before
    native code runs.
*/

#include <stdint.h>
#include "hv_config.h"
#include "nc_abi.h"

uint32_t nc_call_native(uintptr_t fn, uintptr_t params, uint64_t limit);

/* ---- Per core state ---------------------------------------------------------------------------

   TPIDR_EL2 of each core that runs native code points at its entry (hv_entry.S). Offsets are
   shared with the assembler (hv_config.h). */

struct hv_cpu
{
    uint64_t jmpbuf[12];            /* recovery point of nc_call_native */
    uint32_t in_native;
    uint32_t fault_flag;            /* 0, FAULT_CRASH or FAULT_TIMEOUT */
    uint64_t fault_info[3];         /* ESR, FAR, ELR */
    uint32_t captured;              /* helper core: HCR_EL2.TWE is on */
    uint32_t pieces;                /* RUNMP pieces run */
    uint32_t wfe_traps;             /* helper core: idle "wfe" taken */
    uint32_t reserved;
    uint64_t env;                   /* struct NCEnv * for the SDK's runtime (nc_abi.h) */
    uint64_t busy;                  /* counter ticks this core ran native code (NC_OP_STATS) */
    uint32_t calls;                 /* native calls and pieces */
    uint32_t reserved2;
    uint64_t pad[11];
    uint64_t regs[32];              /* x0-x30, sp of the native code at its last fault */
    uint8_t  params[NC_MP_PARAMS_MAX] __attribute__((aligned(16)));
} __attribute__((aligned(64)));

_Static_assert(__builtin_offsetof(struct hv_cpu, jmpbuf) == CPU_JMPBUF, "CPU_JMPBUF");
_Static_assert(__builtin_offsetof(struct hv_cpu, in_native) == CPU_IN_NATIVE, "CPU_IN_NATIVE");
_Static_assert(__builtin_offsetof(struct hv_cpu, fault_flag) == CPU_FAULT_FLAG, "CPU_FAULT_FLAG");
_Static_assert(__builtin_offsetof(struct hv_cpu, fault_info) == CPU_FAULT_INFO, "CPU_FAULT_INFO");
_Static_assert(__builtin_offsetof(struct hv_cpu, env) == NC_CPU_ENV, "NC_CPU_ENV");
_Static_assert(__builtin_offsetof(struct hv_cpu, regs) == CPU_REGS, "CPU_REGS");
_Static_assert(sizeof(struct hv_cpu) == CPU_SIZE, "CPU_SIZE");

struct hv_cpu hv_cpu[4];
/* EL2 stacks of the helper cores (core 0 uses __stack_top) */
uint8_t hv_stacks[4][HV_STACK_SIZE] __attribute__((aligned(4096)));   /* lowest page: guard */

/* Native environment of each core (struct NCEnv, nc_abi.h) and the log writer it points at */
static struct NCEnv hv_env[4];
void hv_log_write(const char *text, uint32_t len);
#ifdef HV_S2
static void s2_init(uint64_t base);
#endif

static inline uint32_t core_id(void)
{
    uint64_t mpidr;
    __asm__ volatile("mrs %0, MPIDR_EL1" : "=r"(mpidr));
    return mpidr & 3;
}

static uint64_t last_fault[3];      /* last fault reported to the 68k, for NC_OP_INFO */

/* ---- EL2 translation tables -------------------------------------------------------------------

   Identity map, 4 KB granule, 34-bit VA (T0SZ = 30), level 1 -> level 2 (2 MB blocks) for every GB:

     0x0_0000_0000 - 0x0_00FF_FFFF  invalid: 68k chip RAM / CIA / custom chips live on the Amiga
                                    bus, not in Pi RAM. Native code touching them faults (and is
                                    caught) instead of corrupting low Pi memory.
     RAM the device tree reports    Normal WB. From the first request on (Emu68 has set up the
                                    memory of the 68k by then) only the RAM the 68k sees 1:1, see
                                    restrict_native_map(): the kernel, JIT and MMU pool of Emu68,
                                    video memory taken from the 68k, RAM above 4 GB are gone
                                    (hypervisor 7).
     0x0_FC00_0000 - 0x0_FFFF_FFFF  Device (BCM2711 peripherals, low-peripheral mode)
     the 2 MB of the hypervisor     4 KB pages (l3_hv): code read-only, page tables read-only,
                                    data read-write, the lowest page of every stack unmapped (a
                                    stack overflow of native code faults instead of overwriting
                                    the hypervisor).

   Cache maintenance alias at MAINT_ALIAS + PA, 0 - 8 GB without the hole (Normal, XN; Device
   for the peripherals). The DC operations of Emu68 must reach every address, including the low
   16 MB: it flushes its kernel image there before moving itself to the top of RAM
   (src/aarch64/start.c:1440). Skipping that flush broke the first boot on the A1200. The
   hypervisor also writes its read-only page tables through it. It is writable, and native code
   could reach it with a 64-bit address: the protection is against accidents (68k pointers are
   32 bits), not against malice.

   Same attributes as the EL1 mappings of Emu68 (Normal WB, inner shareable), so EL1 and EL2 see
   the same cache lines. */

#define PTE_VALID       (1UL << 0)
#define PTE_TABLE       (1UL << 1)              /* level 1/2: next level table */
#define PTE_BLOCK       (0UL << 1)
#define PTE_PAGE        (3UL << 0)              /* level 3: a 4 KB page */
#define PTE_ATTR(n)     ((uint64_t)(n) << 2)    /* MAIR_EL2 index */
#define PTE_RO          (1UL << 7)              /* AP[2]: read-only */
#define PTE_SH_INNER    (3UL << 8)
#define PTE_AF          (1UL << 10)
#define PTE_XN          (1UL << 54)

#define ATTR_NORMAL     0                       /* MAIR attr0 = 0xff */
#define ATTR_DEVICE     1                       /* MAIR attr1 = 0x04, Device-nGnRE */

#define BLOCK_NORMAL    (PTE_VALID | PTE_BLOCK | PTE_ATTR(ATTR_NORMAL) | PTE_SH_INNER | PTE_AF)
#define BLOCK_DEVICE    (PTE_VALID | PTE_BLOCK | PTE_ATTR(ATTR_DEVICE) | PTE_AF | PTE_XN)
#define PAGE_NORMAL     (PTE_PAGE | PTE_ATTR(ATTR_NORMAL) | PTE_SH_INNER | PTE_AF)

#define GB              0x40000000UL
#define MB2             0x200000UL
#define PAGE            0x1000UL

#define SCTLR_M         (1UL << 0)
#define SCTLR_A         (1UL << 1)
#define SCTLR_C         (1UL << 2)
#define SCTLR_I         (1UL << 12)
#define SCTLR_WXN       (1UL << 19)

#define MAINT_ALIAS     0x200000000UL           /* VA of PA 0 for cache maintenance */
#define PA_LIMIT        0x200000000UL           /* 8 GB */

/* All page tables in .bss.pt (hv.ld.S): read-only once the MMU is on */
#define PT_SECTION      __attribute__((aligned(4096), section(".bss.pt")))
uint64_t hv_l1[512] PT_SECTION;                 /* TTBR0_EL2 of all cores */
#define l1 hv_l1
static uint64_t l2_id[8][512] PT_SECTION;       /* identity map, GB n */
static uint64_t l2_alias_gb3[512] PT_SECTION;   /* maintenance alias of GB 3 (Device part) */
static uint64_t l3_hv[512] PT_SECTION;          /* the 2 MB of the hypervisor */
#ifdef HV_RESETSPY
static uint64_t rs_l1[512] PT_SECTION, rs_l2[512] PT_SECTION, rs_l3[512] PT_SECTION;
static void rs_init(void);
static uint32_t rs_emulated, rs_ctlr_writes, rs_last_ctlr;
#endif
#ifdef HV_S2
static uint64_t s2_l1[512] PT_SECTION;          /* stage 2 for EL1 (Emu68) */
static uint64_t s2_l2[512] PT_SECTION;
#endif

/* Layout of the hypervisor (hv.ld.S) */
#pragma GCC visibility push(hidden)
extern char __hv_start[], __ro_end[], __pt_start[], __pt_end[], __stack0_bottom[], __bss_end[], __image_end[];
#pragma GCC visibility pop

static uint32_t ram_gb_mask;     /* bit n: GB n holds RAM the device tree reports */

/* After restrict_native_map(): bit n = the 2 MB at n * 2 MB (below 4 GB) is RAM the 68k sees */
static uint8_t visible_68k[256];
static int restricted;

static inline int gb_has_ram(uint64_t pa)
{
    uint64_t gb = pa >> 30;
    return gb < 8 && ((ram_gb_mask >> gb) & 1);
}

/* RAM in the identity map that requests may point at (68k blocks, modules, params) */
static inline int pa_is_ram(uint64_t pa)
{
    if (pa < 0x01000000UL || (pa >= 0xFC000000UL && pa < 0x100000000UL))
        return 0;
    if (restricted)
        return pa < 0x100000000UL && ((visible_68k[pa >> 24] >> ((pa >> 21) & 7)) & 1);
    return gb_has_ram(pa);
}

/* Reachable through the maintenance alias: RAM, or the peripherals in GB 3 */
static inline int pa_in_alias(uint64_t pa)
{
    return gb_has_ram(pa) || (pa >= 0xFC000000UL && pa < 0x100000000UL);
}

#ifdef DEBUG_LED
static void led_delay(uint64_t ticks)
{
    uint64_t start, now;

    __asm__ volatile("mrs %0, CNTPCT_EL0" : "=r"(start));
    do
        __asm__ volatile("mrs %0, CNTPCT_EL0" : "=r"(now));
    while (now - start < ticks);
}

static void led_set(int on)
{
    volatile uint32_t *gpio = (volatile uint32_t *)(uintptr_t)GPIO_BASE;

    gpio[(on ? GPSET1 : GPCLR1) / 4] = __builtin_bswap32(LED_GPIO_BIT);
}

static void led_flash(int count)
{
    for (int i = 0; i < count; i++)
    {
        led_set(1);
        led_delay(CNT_FREQ / 5);
        led_set(0);
        led_delay(CNT_FREQ / 5);
    }
    led_delay(CNT_FREQ / 2);
}

void hv_led_off(void)
{
    led_set(0);
}
#else
#define led_flash(n)
void hv_led_off(void) {}
#endif

/* The lowest page of each stack: unmapped */
static int is_guard_page(uint64_t a)
{
    if (a == (uintptr_t)__stack0_bottom)
        return 1;
    for (int i = 0; i < 4; i++)
    {
        if (a == (uintptr_t)hv_stacks[i])
            return 1;
#ifdef HV_DIAG_GUARD_HELPERS
        /* Diagnosis: of the helper stacks only the top 8 KB stay mapped, the unused stacks not at
           all: an EL2 access below faults and the panic of that core shows who it was */
        if (a >= (uintptr_t)hv_stacks[i] && a < (uintptr_t)hv_stacks[i] + HV_STACK_SIZE
            && (!((HV_HELPER_MASK >> i) & 1) || a < (uintptr_t)hv_stacks[i] + HV_STACK_SIZE - 2 * PAGE))
            return 1;
#endif
    }
    return 0;
}

/* Runs on core 0 with the MMU off: the tables are written directly */
/* Diagnosis, only in builds with HV_EXTRA=-DHV_DIAG (NOTES.md 26.6, 26.7): every stack is painted
   at boot, NC_OP_INFO logs how deep each one was used and any panic of a core (hv_entry.S keeps
   ESR/ELR/FAR per core). More with -DHV_CANARY (who writes into the hypervisor's 2 MB),
   -DHV_RESETSPY (the state of the 68k when it executes RESET), -DHV_EMU68LOG (the console text
   of Emu68 with async_log), -DHV_S2 (EL1 accesses to the hypervisor). */
#if !defined(HV_DIAG) && !defined(HV_NO_PAINT)
#define HV_NO_PAINT 1
#endif
#ifndef HV_PAINT_VALUE
#define HV_PAINT_VALUE  0xA5A5A5A5A5A5A5A5UL
#endif
/* read from .rodata: the code is the same whatever the value (for the zero-paint experiment).
   HV_PAINT_UNUSED: the stacks no core uses (hv_stacks[0], [2]). */
#ifndef HV_PAINT_UNUSED
#define HV_PAINT_UNUSED HV_PAINT_VALUE
#endif
#ifndef HV_PAINT_CORE0
#define HV_PAINT_CORE0  HV_PAINT_VALUE
#endif
static const volatile uint64_t paint_value = HV_PAINT_VALUE;    /* helper cores 1, 3 */
static const volatile uint64_t paint_unused = HV_PAINT_UNUSED;  /* hv_stacks[0], [2] */
static const volatile uint64_t paint_core0 = HV_PAINT_CORE0;    /* core 0 */
#define STACK_PAINT     (paint_value)
extern uint64_t hv_panic_info[4][4] __attribute__((visibility("hidden")));

#ifdef HV_CANARY
#define CANARY          0xA5A5A5A5A5A5A5A5UL
static uint64_t canary_code_sum;
#endif

static void paint(uint64_t lo, uint64_t hi, uint64_t value)
{
    for (uint64_t a = lo; a < hi; a += 8)
        *(volatile uint64_t *)(uintptr_t)a = value;
}

void hv_init(uint32_t gb_mask)
{
#ifdef HV_CANARY
    /* Diagnosis: who writes into the 2 MB of the hypervisor? Everything unused gets a pattern
       (the stacks in use stay zero, as without this), the code a checksum; canary_report() says
       what changed. MMU off here: plain writes reach it all. */
    paint((uintptr_t)__bss_end, (uintptr_t)__hv_start + MB2, CANARY);
    for (int i = 0; i < 4; i++)
        if (!((HV_HELPER_MASK >> i) & 1))
            paint((uintptr_t)hv_stacks[i], (uintptr_t)hv_stacks[i] + HV_STACK_SIZE, CANARY);
    canary_code_sum = 0;
    for (uint64_t a = (uintptr_t)__hv_start; a < (uintptr_t)__image_end; a += 4)
        canary_code_sum += *(volatile uint32_t *)(uintptr_t)a;
#endif
#ifndef HV_NO_PAINT
    {
        uint64_t sp;

        __asm__ volatile("mov %0, sp" : "=r"(sp));
        paint((uintptr_t)__stack0_bottom + PAGE, sp - 512, paint_core0);
        for (int i = 0; i < 4; i++)
            paint((uintptr_t)hv_stacks[i] + PAGE, (uintptr_t)hv_stacks[i] + HV_STACK_SIZE,
                  (HV_HELPER_MASK >> i) & 1 ? paint_value : paint_unused);
    }
#else
    (void)paint;
#endif

    uint64_t base = (uintptr_t)__hv_start;      /* the 2 MB of the hypervisor (boot.S: 2 MB aligned) */

    led_flash(2);

    ram_gb_mask = gb_mask | 1;

    /* The runtime of the SDK finds this through TPIDR_EL2 (nc_abi.h, struct NCEnv) */
    for (int i = 0; i < 4; i++)
    {
        hv_env[i].magic = NC_ENV_MAGIC;
        hv_env[i].version = NC_HV_VERSION;
        hv_env[i].core = i;
        hv_env[i].log_write = hv_log_write;
        hv_cpu[i].env = (uintptr_t)&hv_env[i];
    }

    /* the hypervisor itself in 4 KB pages */
    for (int i = 0; i < 512; i++)
    {
        uint64_t a = base + (uint64_t)i * PAGE, e;

        if (a < (uintptr_t)__ro_end)
            e = a | PAGE_NORMAL | PTE_RO;                       /* code, read-only data */
        else if (a >= (uintptr_t)__pt_start && a < (uintptr_t)__pt_end)
            e = a | PAGE_NORMAL | PTE_RO | PTE_XN;              /* page tables */
        else if (is_guard_page(a) || a >= (uintptr_t)__bss_end)
            e = 0;
        else
            e = a | PAGE_NORMAL | PTE_XN;                       /* data, stacks */
        l3_hv[i] = e;
    }

    /* identity map: every GB in 2 MB blocks, so restrict_native_map() only clears entries */
    for (int g = 0; g < 8; g++)
    {
        if (g != 3 && !gb_has_ram((uint64_t)g * GB))
        {
            l1[g] = 0;
            continue;
        }
        for (int i = 0; i < 512; i++)
        {
            uint64_t a = (uint64_t)g * GB + (uint64_t)i * MB2, e;

            if (a == base)
                e = (uint64_t)(uintptr_t)l3_hv | PTE_VALID | PTE_TABLE;
            else if (a >= 0xFC000000UL && a < 0x100000000UL)
                e = a | BLOCK_DEVICE;
            else if (a < 0x01000000UL || !gb_has_ram(a))
                e = 0;
            else
                e = a | BLOCK_NORMAL;
            l2_id[g][i] = e;
        }
        l1[g] = (uint64_t)(uintptr_t)l2_id[g] | PTE_VALID | PTE_TABLE;
    }

    /* Maintenance alias: L1 entries 8-15 = PA 0-8 GB. GB 3 needs 2 MB blocks for the Device part. */
    for (int i = 0; i < 512; i++)
    {
        uint64_t a = 3 * GB + (uint64_t)i * MB2;

        l2_alias_gb3[i] = (a >= 0xFC000000UL) ? (a | BLOCK_DEVICE)
                        : gb_has_ram(a) ? (a | BLOCK_NORMAL | PTE_XN) : 0;
    }
    for (int i = 0; i < 8; i++)
    {
        if (i == 3)
            l1[8 + i] = (uint64_t)(uintptr_t)l2_alias_gb3 | PTE_VALID | PTE_TABLE;
        else
            l1[8 + i] = gb_has_ram((uint64_t)i * GB) ? ((uint64_t)i * GB | BLOCK_NORMAL | PTE_XN) : 0;
    }

#ifdef HV_S2
    s2_init(base);
#endif
#ifdef HV_RESETSPY
    rs_init();
#endif

    uint64_t sctlr;

    __asm__ volatile(
        "dsb sy\n"
        "msr MAIR_EL2, %0\n"
        "msr TCR_EL2, %1\n"
        "msr TTBR0_EL2, %2\n"
        "isb\n"
        "tlbi alle2\n"
        "dsb sy\n"
        "isb\n"
        :: "r"((uint64_t)MAIR_EL2_VAL), "r"((uint64_t)TCR_EL2_VAL), "r"((uint64_t)(uintptr_t)l1) : "memory");

    __asm__ volatile("mrs %0, SCTLR_EL2" : "=r"(sctlr));
    sctlr |= SCTLR_M | SCTLR_C | SCTLR_I;
    sctlr &= ~(SCTLR_A | SCTLR_WXN);
    __asm__ volatile("msr SCTLR_EL2, %0\n isb" :: "r"(sctlr) : "memory");
    __asm__ volatile("ic iallu\n dsb sy\n isb" ::: "memory");

#ifdef HV_S2
    /* stage 2 for core 0 (tables written above, before the MMU made them read-only);
       HCR_EL2.VM is set by hv_entry before entering Emu68 */
    __asm__ volatile("dsb sy\n msr VTCR_EL2, %0\n msr VTTBR_EL2, %1\n isb\n tlbi vmalls12e1\n dsb sy\n isb"
                     :: "r"((uint64_t)VTCR_EL2_VAL), "r"((uint64_t)(uintptr_t)s2_l1) : "memory");
#endif

#ifdef HV_RESETSPY
    /* stage 2 for core 0 only; HCR_EL2.VM is set by hv_entry */
    __asm__ volatile("dsb sy\n msr VTCR_EL2, %0\n msr VTTBR_EL2, %1\n isb\n tlbi vmalls12e1\n dsb sy\n isb"
                     :: "r"((uint64_t)VTCR_EL2_VAL), "r"((uint64_t)(uintptr_t)rs_l1) : "memory");
#endif

    led_flash(3);
}

/* ---- Helpers ---------------------------------------------------------------------------------- */

static uint32_t stat_traps, stat_calls, stat_faults, stat_skipped;

#define PA_INVALID      (~0UL)

/* EL1 virtual address -> physical address using Emu68's own stage 1 tables. PA_INVALID on failure. */
static uint64_t el1_to_pa(uint64_t va)
{
    uint64_t saved, par;

    __asm__ volatile("mrs %0, PAR_EL1" : "=r"(saved));
    __asm__ volatile("at s1e1r, %1\n isb\n mrs %0, PAR_EL1" : "=r"(par) : "r"(va) : "memory");
    __asm__ volatile("msr PAR_EL1, %0" :: "r"(saved));

    if (par & 1)
        return PA_INVALID;

    return (par & 0x0000fffffffff000UL) | (va & 0xfff);
}

/* 68k address range -> physical, only if it is contiguous RAM (68k Fast RAM is mapped 1:1) */
static uint64_t m68k_range_to_pa(uint32_t addr, uint32_t len)
{
    uint64_t pa = el1_to_pa(addr);

    if (pa == PA_INVALID || !pa_is_ram(pa))
        return 0;
    if (len > 1)
    {
        uint64_t last = el1_to_pa((uint64_t)addr + len - 1);
        if (last != pa + len - 1 || !pa_is_ram(last))
            return 0;
    }
    return pa;
}

static void sync_icache(uint64_t pa, uint32_t len)
{
    uint64_t ctr;
    __asm__ volatile("mrs %0, CTR_EL0" : "=r"(ctr));
    uint64_t dline = 4UL << ((ctr >> 16) & 15);
    uint64_t iline = 4UL << (ctr & 15);
    uint64_t end = pa + len;

    for (uint64_t a = pa & ~(dline - 1); a < end; a += dline)
        __asm__ volatile("dc cvau, %0" :: "r"(a) : "memory");
    __asm__ volatile("dsb ish" ::: "memory");
    for (uint64_t a = pa & ~(iline - 1); a < end; a += iline)
        __asm__ volatile("ic ivau, %0" :: "r"(a) : "memory");
    __asm__ volatile("dsb ish\n isb" ::: "memory");
}

static inline uint64_t counter(void)
{
    uint64_t t;
    __asm__ volatile("isb\n mrs %0, CNTPCT_EL0" : "=r"(t));
    return t;
}

/* ---- Log (hypervisor 7, nc_abi.h) -------------------------------------------------------------

   One ring for all cores. Writers: native code through struct NCEnv (the SDK's nc_printf) and the
   hypervisor itself. The reader is NC_OP_LOGREAD on core 0. */

static struct
{
    volatile uint32_t head;         /* bytes written since the Pi started */
    volatile uint32_t lock;
    char buf[NC_LOG_SIZE];
} hv_log;



/* The text goes to the stack first: a bad pointer from native code faults there, before the lock
   is taken. FIQs (the time limit) are masked while the lock is held, so a time-out can never
   leave it taken; a pending one comes right after. */
void hv_log_write(const char *text, uint32_t len)
{
    char tmp[256];
    const volatile char *s = text;
    uint64_t daif;
    uint32_t h, i;

    if (len > sizeof(tmp))
        len = sizeof(tmp);
    for (i = 0; i < len; i++)
        tmp[i] = s[i];

    __asm__ volatile("mrs %0, DAIF\n msr DAIFSet, #1" : "=r"(daif) :: "memory");
    while (__atomic_exchange_n(&hv_log.lock, 1, __ATOMIC_ACQUIRE))
        __asm__ volatile("yield");
    h = hv_log.head;
    for (i = 0; i < len; i++)
        hv_log.buf[(h + i) & (NC_LOG_SIZE - 1)] = tmp[i];
    __atomic_store_n(&hv_log.head, h + len, __ATOMIC_RELEASE);
    __atomic_store_n(&hv_log.lock, 0, __ATOMIC_RELEASE);
    __asm__ volatile("msr DAIF, %0" :: "r"(daif) : "memory");
}

/* A line of the hypervisor's own */
struct line
{
    char b[160];
    uint32_t n;
};

static void put_s(struct line *l, const char *s)
{
    while (*s && l->n < sizeof(l->b))
        l->b[l->n++] = *s++;
}

static void put_hex(struct line *l, uint64_t v, int digits)
{
    while (digits-- > 0 && l->n < sizeof(l->b))
        l->b[l->n++] = "0123456789abcdef"[(v >> (4 * digits)) & 15];
}

/* ---- Fault details (NC_OP_FAULTINFO) ---------------------------------------------------------- */

static struct NCFaultInfo fault_rec;
static volatile uint32_t fault_lock;
static uint32_t fault_count;

extern char __stack_top[] __attribute__((visibility("hidden")));

/* Offset of addr in the module's image, NC_FAULT_NOOFFSET outside it (modules are < 1 MB) */
static uint32_t mod_offset(uint64_t addr, uint64_t mod_pa)
{
    return (mod_pa && addr >= mod_pa && addr - mod_pa < 0x100000) ? (uint32_t)(addr - mod_pa) : NC_FAULT_NOOFFSET;
}

/* The native code of this core faulted or ran out of time (c->fault_flag): keep the details for
   NC_OP_FAULTINFO and say so in the log. With several cores at once the first one is kept. */
static void record_fault(struct hv_cpu *c, uint32_t core, uint64_t mod_pa, uint32_t mod68k)
{
    struct NCFaultInfo *f = &fault_rec;
    uint64_t lo, hi, fp, elr = c->fault_info[2];
    uint32_t n;
    struct line l;

    if (__atomic_exchange_n(&fault_lock, 1, __ATOMIC_ACQUIRE))
        return;

    f->version = 1;
    f->status = c->fault_flag == FAULT_TIMEOUT ? NC_ST_TIMEOUT : NC_ST_FAULT;
    f->core = core;
    f->module = mod68k;
    f->count = ++fault_count;
    f->esr = (uint32_t)c->fault_info[0];
    f->far_hi = c->fault_info[1] >> 32;
    f->far_lo = (uint32_t)c->fault_info[1];
    f->elr_hi = elr >> 32;
    f->elr_lo = (uint32_t)elr;
    f->sp_hi = c->regs[31] >> 32;
    f->sp_lo = (uint32_t)c->regs[31];
    for (n = 0; n < 31; n++)
    {
        f->x[n][0] = c->regs[n] >> 32;
        f->x[n][1] = (uint32_t)c->regs[n];
    }

    /* backtrace: where, the link register, then the frame records { fp, lr } on this core's stack */
    if (core == 0)
    {
        hi = (uintptr_t)__stack_top;
        lo = hi - HV_STACK_SIZE;
    }
    else
    {
        lo = (uintptr_t)hv_stacks[core];
        hi = lo + HV_STACK_SIZE;
    }
    lo += PAGE;                                 /* the guard page is not mapped */
    f->backtrace[0] = mod_offset(elr, mod_pa);
    f->backtrace[1] = mod_offset(c->regs[30], mod_pa);
    n = 2;
    for (fp = c->regs[29]; n < NC_FAULT_FRAMES && fp >= lo && fp + 16 <= hi && !(fp & 7); n++)
    {
        uint64_t next = ((volatile uint64_t *)(uintptr_t)fp)[0];

        f->backtrace[n] = mod_offset(((volatile uint64_t *)(uintptr_t)fp)[1], mod_pa);
        if (next <= fp)
        {
            n++;
            break;
        }
        fp = next;
    }
    for (; n < NC_FAULT_FRAMES; n++)
        f->backtrace[n] = NC_FAULT_NOOFFSET;

    __atomic_store_n(&fault_lock, 0, __ATOMIC_RELEASE);

    l.n = 0;
    put_s(&l, c->fault_flag == FAULT_TIMEOUT ? "nc: time-out on core " : "nc: fault on core ");
    put_hex(&l, core, 1);
    if (c->fault_flag != FAULT_TIMEOUT)
    {
        put_s(&l, ", ESR ");
        put_hex(&l, f->esr, 8);
        put_s(&l, ", FAR ");
        put_hex(&l, c->fault_info[1], 16);
    }
    put_s(&l, ", module ");
    put_hex(&l, mod68k, 8);
    put_s(&l, " +");
    put_hex(&l, f->backtrace[0], 8);
    put_s(&l, "\n");
    hv_log_write(l.b, l.n);
}

static void put_dec(struct line *l, uint32_t v)
{
    char tmp[10];
    int n = 0;

    do
        tmp[n++] = '0' + v % 10;
    while ((v /= 10) && n < 10);
    while (n && l->n < sizeof(l->b))
        l->b[l->n++] = tmp[--n];
}

/* ---- Native code sees the RAM of the 68k only (hypervisor 7) ----------------------------------

   Runs once, at the first request. By then Emu68 has mapped the memory of the 68k 1:1 at EL1
   (start.c:1362) and keeps its kernel, JIT and MMU pool at high virtual addresses. Every 2 MB
   block of RAM that EL1 does not map 1:1 (checked at its start and in its middle: Emu68 takes
   memory away in 1 MB steps) leaves the identity map, RAM above 4 GB always. Its last 2 MB of
   68k RAM is followed by those private areas: a kernel that writes past the end of a buffer
   there now faults instead of corrupting Emu68. Only valid -> invalid changes, so no
   break-before-make; the tables are read-only and written through the maintenance alias. */
static void restrict_native_map(void)
{
    uint32_t visible = 0, hidden = 0;
    struct line l;

    for (int g = 0; g < 8; g++)
    {
        if (!(l1[g] & PTE_VALID))
            continue;
        for (int i = 0; i < 512; i++)
        {
            uint64_t e = l2_id[g][i], a = (uint64_t)g * GB + (uint64_t)i * MB2;

            if (!(e & PTE_VALID) || (e & PTE_TABLE) || ((e >> 2) & 7) != ATTR_NORMAL)
                continue;           /* unmapped, the hypervisor itself, Device */
            if (a < 0x100000000UL && (el1_to_pa(a) == a || el1_to_pa(a + MB2 / 2) == a + MB2 / 2))
            {
                visible_68k[a >> 24] |= 1 << ((a >> 21) & 7);
                visible++;
            }
            else
            {
                *(volatile uint64_t *)(uintptr_t)(MAINT_ALIAS + (uintptr_t)&l2_id[g][i]) = 0;
                hidden++;
            }
        }
    }
    __asm__ volatile("dsb ishst\n tlbi alle2is\n dsb ish\n isb" ::: "memory");
    restricted = 1;

    l.n = 0;
    put_s(&l, "nc: native code sees the RAM of the 68k only: ");
    put_dec(&l, visible * 2);
    put_s(&l, " MB, ");
    put_dec(&l, hidden * 2);
    put_s(&l, " MB hidden\n");
    hv_log_write(l.b, l.n);
}

/* Diagnosis: the deepest use of each stack (painted in hv_init) and panics, into the log */
static uint32_t stack_used(uint64_t lo, uint64_t hi)
{
    uint64_t a = lo + PAGE;

    while (a < hi && *(volatile uint64_t *)(uintptr_t)a == STACK_PAINT)
        a += 8;
    return (uint32_t)(hi - a);
}

/* Diagnosis: the console text of Emu68. With "async_log" in cmdline.txt Emu68 queues every
   kprintf in a 16 MB buffer (q_buffer, src/raspi/support_rpi.c) of its private memory. It survives
   a reset of the Amiga (the Pi keeps running): find the longest run of text in the RAM the 68k
   does not see and copy its end into the log. */
#ifdef HV_EMU68LOG
static int is_text(uint8_t c)
{
    return (c >= 0x20 && c < 0x7f) || c == '\n' || c == '\r' || c == '\t';
}

static void emu68_log_dump(void)
{
    uint64_t run_start = 0, run_len = 0, best_start = 0, best_len = 0, prev_end = 0;
    uint32_t run_lines = 0;     /* "\n[A-Z" in the run: lines of kprintf ("[BOOT] ...", "[JIT] ...") */
    uint8_t prev = 0, prev2 = 0;
    struct line l;

    for (uint64_t pa = 0x01000000UL; pa < 0xFC000000UL; pa += MB2)
    {
        if (!gb_has_ram(pa) || (pa >= (uintptr_t)__hv_start && pa < (uintptr_t)__hv_start + MB2))
            continue;
        if (restricted && ((visible_68k[pa >> 24] >> ((pa >> 21) & 7)) & 1))
            continue;                               /* RAM of the 68k */
        if (pa != prev_end)
            run_len = 0;                            /* not contiguous with the last block */
        prev_end = pa + MB2;

        const volatile uint8_t *p = (const volatile uint8_t *)(uintptr_t)(MAINT_ALIAS + pa);
        for (uint64_t i = 0; i < MB2; i++)
        {
            uint8_t c = p[i];

            if (is_text(c))
            {
                if (!run_len)
                {
                    run_start = pa + i;
                    run_lines = 0;
                }
                if (prev2 == '\n' && prev == '[' && c >= 'A' && c <= 'Z')
                    run_lines++;
                run_len++;
                if (run_lines >= 3 && run_len > best_len)
                {
                    best_len = run_len;
                    best_start = run_start;
                }
            }
            else
                run_len = 0;
            prev2 = prev;
            prev = c;
        }
    }

    l.n = 0;
    put_s(&l, "nc diag: Emu68 text, longest run ");
    put_dec(&l, (uint32_t)best_len);
    put_s(&l, " bytes at ");
    put_hex(&l, best_start, 9);
    put_s(&l, best_len >= 256 ? ", its end:\n" : ": none\n");
    hv_log_write(l.b, l.n);
    if (best_len < 256)
        return;

    uint64_t n = best_len < 3000 ? best_len : 3000, a = best_start + best_len - n;
    char chunk[128];
    uint32_t k = 0;

    for (uint64_t i = 0; i < n; i++)
    {
        char c = ((const volatile char *)(uintptr_t)(MAINT_ALIAS + a))[i];

        if (c == '\r')
            continue;
        chunk[k++] = c;
        if (k == sizeof(chunk))
        {
            hv_log_write(chunk, k);
            k = 0;
        }
    }
    chunk[k++] = '\n';
    hv_log_write(chunk, k);
}
#endif

#ifdef HV_CANARY
/* Words in [lo, hi) that are not `expect`, read through the maintenance alias (guard pages and
   the tail of the 2 MB are not in the identity map). Logs the range that changed. */
static uint32_t canary_scan(const char *name, uint64_t lo, uint64_t hi, uint64_t expect)
{
    uint64_t first = 0, last = 0, first_val = 0;
    uint32_t n = 0;
    struct line l;

    for (uint64_t a = lo; a < hi; a += 8)
    {
        uint64_t v = *(volatile uint64_t *)(uintptr_t)(MAINT_ALIAS + a);

        if (v == expect)
            continue;
        if (!n)
        {
            first = a;
            first_val = v;
        }
        last = a;
        n++;
    }
    if (!n)
        return 0;
    l.n = 0;
    put_s(&l, "nc canary: ");
    put_s(&l, name);
    put_s(&l, ": ");
    put_dec(&l, n);
    put_s(&l, " words changed, +");
    put_hex(&l, first - (uintptr_t)__hv_start, 6);
    put_s(&l, " .. +");
    put_hex(&l, last - (uintptr_t)__hv_start, 6);
    put_s(&l, ", first = ");
    put_hex(&l, first_val, 16);
    put_s(&l, "\n");
    hv_log_write(l.b, l.n);
    return 1;
}

static void canary_report(void)
{
    uint32_t changed = 0;
    uint64_t sum = 0;
    struct line l;

    changed += canary_scan("tail", (uintptr_t)__bss_end, (uintptr_t)__hv_start + MB2, CANARY);
    changed += canary_scan("unused stack 0", (uintptr_t)hv_stacks[0], (uintptr_t)hv_stacks[0] + HV_STACK_SIZE, CANARY);
    changed += canary_scan("unused stack 2", (uintptr_t)hv_stacks[2], (uintptr_t)hv_stacks[2] + HV_STACK_SIZE, CANARY);
    /* stacks in use: nothing may ever be below their top 8 KB */
    changed += canary_scan("stack core 1 (low part)", (uintptr_t)hv_stacks[1], (uintptr_t)hv_stacks[1] + HV_STACK_SIZE - 2 * PAGE, 0);
    changed += canary_scan("stack core 3 (low part)", (uintptr_t)hv_stacks[3], (uintptr_t)hv_stacks[3] + HV_STACK_SIZE - 2 * PAGE, 0);
    changed += canary_scan("stack core 0 (low part)", (uintptr_t)__stack0_bottom, (uintptr_t)__stack_top - 2 * PAGE, 0);
    for (uint64_t a = (uintptr_t)__hv_start; a < (uintptr_t)__image_end; a += 4)
        sum += *(volatile uint32_t *)(uintptr_t)a;

    l.n = 0;
    put_s(&l, "nc canary: ");
    put_dec(&l, changed);
    put_s(&l, " regions changed, code ");
    put_s(&l, sum == canary_code_sum ? "intact\n" : "CHANGED\n");
    hv_log_write(l.b, l.n);
}
#endif

__attribute__((unused)) static void diag_log(void)
{
    struct line l;
#ifdef HV_CANARY
    canary_report();
#endif
#ifdef HV_RESETSPY
    l.n = 0;
    put_s(&l, "nc reset spy: ");
    put_dec(&l, rs_emulated);
    put_s(&l, " GIC accesses emulated, ");
    put_dec(&l, rs_ctlr_writes);
    put_s(&l, " writes to GICC_CTLR, last ");
    put_hex(&l, rs_last_ctlr, 8);
    put_s(&l, "\n");
    hv_log_write(l.b, l.n);
#endif

    l.n = 0;
    put_s(&l, "nc diag: stack bytes used, core 0 ");
    put_dec(&l, stack_used((uintptr_t)__stack0_bottom, (uintptr_t)__stack_top));
    for (int i = 1; i < 4; i++)
    {
        put_s(&l, i == 1 ? ", 1 " : i == 2 ? ", 2 " : ", 3 ");
#ifdef HV_DIAG_GUARD_HELPERS
        if (!((HV_HELPER_MASK >> i) & 1))
            put_dec(&l, 0);                 /* not mapped in this build */
        else
            put_dec(&l, stack_used((uintptr_t)hv_stacks[i] + HV_STACK_SIZE - 3 * PAGE, (uintptr_t)hv_stacks[i] + HV_STACK_SIZE));
#else
        put_dec(&l, stack_used((uintptr_t)hv_stacks[i], (uintptr_t)hv_stacks[i] + HV_STACK_SIZE));
#endif
    }
    put_s(&l, " (of ");
    put_dec(&l, HV_STACK_SIZE - PAGE);
    put_s(&l, ")\n");
    hv_log_write(l.b, l.n);

    for (int i = 0; i < 4; i++)
    {
        if (!hv_panic_info[i][3])
            continue;
        l.n = 0;
        put_s(&l, "nc diag: PANIC on core ");
        put_hex(&l, i, 1);
        put_s(&l, " x");
        put_dec(&l, (uint32_t)hv_panic_info[i][3]);
        put_s(&l, ": ESR ");
        put_hex(&l, hv_panic_info[i][0], 8);
        put_s(&l, " ELR ");
        put_hex(&l, hv_panic_info[i][1], 16);
        put_s(&l, " FAR ");
        put_hex(&l, hv_panic_info[i][2], 16);
        put_s(&l, "\n");
        hv_log_write(l.b, l.n);
    }
}

/* ---- Requests --------------------------------------------------------------------------------- */

/* fn of module, params_size bytes at params (0: none): checked, physical addresses out.
   Returns NC_ST_OK or the error status. */
static uint32_t check_call(uint32_t fn, uint32_t params, uint32_t params_size, uint32_t module,
                           uint64_t *fn_pa, uint64_t *params_pa, uint64_t *module_pa)
{
    uint64_t mod_pa = m68k_range_to_pa(module, 12);
    *module_pa = mod_pa;
    if (!mod_pa || (mod_pa & 15))
        return NC_ST_BADMOD;

    volatile struct NativeHeader *h = (volatile struct NativeHeader *)(uintptr_t)mod_pa;
    uint32_t count = h->entry_count;
    if (h->magic != NARM_MAGIC || h->version != NARM_VERSION || count == 0 || count > NARM_MAX_ENTRIES
        || !m68k_range_to_pa(module, 12 + 4 * count))
        return NC_ST_BADMOD;

    /* The function must be one of the module's entries: no jumps to arbitrary addresses */
    int found = 0;
    for (uint32_t i = 0; i < count && !found; i++)
        found = (module + h->entries[i] == fn);

    *fn_pa = m68k_range_to_pa(fn, 4);
    if (!found || (fn & 15) || !*fn_pa)
        return NC_ST_BADENTRY;

    *params_pa = 0;
    if (params != 0)
    {
        *params_pa = m68k_range_to_pa(params, params_size);
        if (!*params_pa)
            return NC_ST_BADADDR;
    }
    return NC_ST_OK;
}

static uint32_t fault_status(uint32_t kind)
{
    return kind == FAULT_TIMEOUT ? NC_ST_TIMEOUT : NC_ST_FAULT;
}

static void report_fault(volatile struct NCBlock *b, const uint64_t *info, uint32_t kind)
{
    stat_faults++;
    for (int i = 0; i < 3; i++)
    {
        last_fault[i] = info[i];
        b->info[2 * i] = info[i] >> 32;
        b->info[2 * i + 1] = (uint32_t)info[i];
    }
    b->status = fault_status(kind);
}

/* Time limit of one native call (one piece of a job) in counter ticks, from the request's
   info[NC_CALLINFO_TIMEOUT] in microseconds (0: the default) */
static uint64_t limit_ticks(volatile struct NCBlock *b)
{
    uint64_t us = b->info[NC_CALLINFO_TIMEOUT], freq;

    if (us == 0)
        us = NC_TIMEOUT_DEFAULT_US;
    __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(freq));
    return us * freq / 1000000;
}

static void do_call(volatile struct NCBlock *b)
{
    struct hv_cpu *c = &hv_cpu[0];
    uint64_t fn_pa, params_pa, mod_pa, t0, t1, freq;

    stat_calls++;
    b->result = 0xffffffff;

    uint64_t limit = limit_ticks(b);
    uint32_t st = check_call(b->arg0, b->arg1, 4, b->arg2, &fn_pa, &params_pa, &mod_pa);
    if (st != NC_ST_OK)
    {
        b->status = st;
        return;
    }

    c->fault_flag = 0;
    __asm__ volatile("isb\n mrs %0, CNTPCT_EL0" : "=r"(t0));
    uint32_t r = nc_call_native(fn_pa, params_pa, limit);
    __asm__ volatile("isb\n mrs %0, CNTPCT_EL0" : "=r"(t1));
    __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(freq));

    c->busy += t1 - t0;
    c->calls++;

    /* Native run time (the 68k cannot measure it: its interrupts wait meanwhile) */
    b->info[NC_CALLINFO_TICKS] = (uint32_t)(t1 - t0);
    b->info[NC_CALLINFO_FREQ] = (uint32_t)freq;

    if (c->fault_flag)
    {
        record_fault(c, 0, mod_pa, b->arg2);
        report_fault(b, c->fault_info, c->fault_flag);
        return;
    }

    b->result = r;
    b->status = NC_ST_OK;
}

/* ---- RUNMP and asynchronous jobs: one kernel over a range on several cores ------------------

   A job hands out pieces of [from, to) from one counter; every core that takes a piece runs
   it on its own copy of the params (NCRange = the piece) and adds up the result.

   RUNMP (synchronous): core 0 posts the job and takes pieces itself; idle helper cores join
   from their trapped "wfe" (helper_sync). A helper that never comes (Emu68 gave it work, e.g.
   ppc_enable) takes nothing, so core 0 never waits for it: only for the pieces a helper
   actually took. job.busy counts the helpers inside; a helper registers, then checks
   job.state, and core 0 clears the state before it waits for busy == 0. Either the helper is
   counted before core 0 looks, or it sees the job closed (or the next one, which is complete
   before it is published).

   START / WAIT (asynchronous, hypervisor 4): START puts the job into a free slot, wakes the
   helpers and returns at once: the 68k goes on, the helpers work. Each finished (or, after a
   fault, skipped) piece counts down job.pending; the core that reaches 0 writes the result
   into the 68k's status block (NCJobStatus, done last) and marks the job complete. WAIT with
   the job still running lets core 0 take pieces for up to 0.5 ms and returns
   NC_ST_RUNNING; with the job complete it waits for busy == 0, returns the result and frees
   the slot. The trap of WAIT is also the barrier that makes the helpers' output visible in
   order to the 68k. */

#define JOB_FREE        0
#define JOB_ACTIVE      1
#define JOB_COMPLETE    2

#define JOB_CANCELLED   0x100   /* hv_job.fault of a cancelled job (a real fault: 1 + the core) */

struct hv_job
{
    volatile uint32_t state;
    volatile uint32_t busy;         /* helpers inside the job */
    volatile uint64_t next;         /* next unit to hand out */
    volatile uint32_t pending;      /* async: pieces not finished yet */
    volatile uint32_t sum;
    volatile uint32_t fault;        /* 0, or 1 + the core that faulted first */
    volatile uint32_t cores;        /* bit n: core n ran a piece */
    volatile uint32_t helped;       /* pieces run by helper cores */
    uint32_t async;
    uint32_t to, chunk, params_size;
    uint32_t fault_kind;            /* FAULT_CRASH / FAULT_TIMEOUT of the first fault */
    uint64_t limit;                 /* time limit of one piece, counter ticks */
    uint64_t mod_pa;                /* the module, for the fault details */
    uint32_t mod68k;
    uint64_t fn_pa;
    uint64_t status_pa;             /* async: the 68k's struct NCJobStatus */
    uint64_t fault_info[3];
    uint8_t  params[NC_MP_PARAMS_MAX] __attribute__((aligned(16)));
};

static struct hv_job job;                       /* RUNMP */
static struct hv_job ajob[NC_ASYNC_JOBS];       /* START / WAIT */

static volatile uint32_t helpers_ready;     /* bit n: helper core n is captured */

static void copy_words(void *dst, const volatile void *src, uint32_t size)
{
    uint32_t *d = dst;
    const volatile uint32_t *s = src;

    for (uint32_t i = 0; i < size / 4; i++)
        d[i] = s[i];
}

/* The last piece of an async job is done: tell the 68k (status block, done word last) */
static void complete(struct hv_job *j)
{
    if (j->status_pa)
    {
        volatile struct NCJobStatus *s = (volatile struct NCJobStatus *)(uintptr_t)j->status_pa;

        s->result = j->sum;
        s->status = j->fault == JOB_CANCELLED ? NC_ST_CANCELLED : j->fault ? fault_status(j->fault_kind) : NC_ST_OK;
        s->cores = j->cores;
        __asm__ volatile("dmb ish" ::: "memory");
        s->done = 1;
        __asm__ volatile("dsb ish" ::: "memory");
    }
    __atomic_store_n(&j->state, JOB_COMPLETE, __ATOMIC_SEQ_CST);
}

/* Take pieces until there are none left (or, deadline != 0, until the counter passes it) */
static void run_pieces(struct hv_job *j, struct hv_cpu *c, uint32_t core, uint64_t deadline)
{
    for (;;)
    {
        if (deadline && counter() >= deadline)
            break;
        if (!j->async && __atomic_load_n(&j->fault, __ATOMIC_SEQ_CST))
            break;
        uint64_t n = __atomic_fetch_add(&j->next, j->chunk, __ATOMIC_SEQ_CST);
        if (n >= j->to)
            break;
        uint32_t from = (uint32_t)n;
        uint32_t to = (n + j->chunk > j->to) ? j->to : from + j->chunk;

        /* after a fault an async job is only counted down (skipped pieces), so it completes */
        if (!__atomic_load_n(&j->fault, __ATOMIC_SEQ_CST))
        {
            copy_words(c->params, j->params, j->params_size);
            ((uint32_t *)c->params)[0] = from;
            ((uint32_t *)c->params)[1] = to;

            c->fault_flag = 0;
            uint64_t t0 = counter();
            uint32_t r = nc_call_native(j->fn_pa, (uintptr_t)c->params, j->limit);
            c->busy += counter() - t0;
            c->calls++;
            if (c->fault_flag)
            {
                uint32_t none = 0;
                if (__atomic_compare_exchange_n(&j->fault, &none, core + 1, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
                {
                    for (int i = 0; i < 3; i++)
                        j->fault_info[i] = c->fault_info[i];
                    j->fault_kind = c->fault_flag;
                    record_fault(c, core, j->mod_pa, j->mod68k);
                }
            }
            else
            {
                __atomic_fetch_add(&j->sum, r, __ATOMIC_SEQ_CST);
                __atomic_fetch_or(&j->cores, 1u << core, __ATOMIC_SEQ_CST);
                c->pieces++;
                if (core != 0)
                    __atomic_fetch_add(&j->helped, 1, __ATOMIC_SEQ_CST);
            }
        }
        if (j->async && __atomic_sub_fetch(&j->pending, 1, __ATOMIC_SEQ_CST) == 0)
            complete(j);
        if (!j->async && c->fault_flag)
            break;
    }
}

/* Register in an active job, run pieces, leave. Returns 0 if the job was not active. */
static int join(struct hv_job *j, uint32_t core, uint64_t deadline)
{
    if (__atomic_load_n(&j->state, __ATOMIC_SEQ_CST) != JOB_ACTIVE)
        return 0;
    __atomic_fetch_add(&j->busy, 1, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&j->state, __ATOMIC_SEQ_CST) == JOB_ACTIVE)
    {
        __asm__ volatile("isb" ::: "memory");       /* module code may be new (SYNC on core 0) */
        run_pieces(j, &hv_cpu[core], core, deadline);
    }
    __atomic_fetch_sub(&j->busy, 1, __ATOMIC_SEQ_CST);
    return 1;
}

/* Helper core: the synchronous job first (core 0 waits for it with the 68k stopped), then the
   async ones. Returns 0 if there was nothing to do. */
static int help(uint32_t core)
{
    int did = join(&job, core, 0);

    for (int i = 0; i < NC_ASYNC_JOBS; i++)
        if (__atomic_load_n(&job.state, __ATOMIC_SEQ_CST) != JOB_ACTIVE)
            did |= join(&ajob[i], core, 0);
    return did;
}

/* RUNMP and START: check the request and fill in the job. Returns NC_ST_OK or the error. */
static uint32_t setup_job(volatile struct NCBlock *b, struct hv_job *j, int async)
{
    uint64_t fn_pa, params_pa, mod_pa, status_pa = 0;
    uint32_t from = b->info[NC_MPINFO_FROM], to = b->info[NC_MPINFO_TO];
    uint32_t chunk = b->info[NC_MPINFO_CHUNK], size = b->info[NC_MPINFO_SIZE];

    if (size < 8 || size > NC_MP_PARAMS_MAX || (size & 3) || chunk == 0 || from > to || b->arg1 == 0)
        return NC_ST_BADADDR;
    uint32_t st = check_call(b->arg0, b->arg1, size, b->arg2, &fn_pa, &params_pa, &mod_pa);
    if (st != NC_ST_OK)
        return st;
    if (async)
    {
        status_pa = m68k_range_to_pa(b->info[NC_JOBINFO_STATUS], sizeof(struct NCJobStatus));
        if (!status_pa || (status_pa & 3))
            return NC_ST_BADADDR;
    }

    copy_words(j->params, (volatile void *)(uintptr_t)params_pa, size);
    j->limit = limit_ticks(b);
    j->fault_kind = 0;
    j->mod_pa = mod_pa;
    j->mod68k = b->arg2;
    j->params_size = size;
    j->fn_pa = fn_pa;
    j->status_pa = status_pa;
    j->to = to;
    j->chunk = chunk;
    j->next = from;
    j->pending = (uint32_t)(((uint64_t)to - from + chunk - 1) / chunk);
    j->sum = 0;
    j->fault = 0;
    j->cores = 0;
    j->helped = 0;
    j->async = async;
    return NC_ST_OK;
}

static void do_runmp(volatile struct NCBlock *b)
{
    uint64_t t0, t1, freq;

    stat_calls++;
    b->result = 0xffffffff;

    uint32_t st = setup_job(b, &job, 0);
    if (st != NC_ST_OK)
    {
        b->status = st;
        return;
    }

    t0 = counter();
    __atomic_store_n(&job.state, JOB_ACTIVE, __ATOMIC_SEQ_CST);
    __asm__ volatile("dsb ish\n sev" ::: "memory");

    run_pieces(&job, &hv_cpu[0], 0, 0);

    __atomic_store_n(&job.state, JOB_FREE, __ATOMIC_SEQ_CST);
    while (__atomic_load_n(&job.busy, __ATOMIC_SEQ_CST))
        __asm__ volatile("yield");

    t1 = counter();
    __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(freq));
    b->info[NC_CALLINFO_TICKS] = (uint32_t)(t1 - t0);
    b->info[NC_CALLINFO_FREQ] = (uint32_t)freq;

    if (job.fault)
    {
        report_fault(b, job.fault_info, job.fault_kind);
        return;
    }
    b->info[NC_MPINFO_CORES] = job.cores;
    b->info[NC_MPINFO_HELPED] = job.helped;
    b->result = job.sum;
    b->status = NC_ST_OK;
}

static void do_start(volatile struct NCBlock *b)
{
    struct hv_job *j = 0;
    uint32_t slot;

    stat_calls++;
    b->result = 0xffffffff;

    for (slot = 0; slot < NC_ASYNC_JOBS; slot++)
        if (ajob[slot].state == JOB_FREE)
        {
            j = &ajob[slot];
            break;
        }
    if (!j)
    {
        b->status = NC_ST_BUSY;
        return;
    }
    uint32_t st = setup_job(b, j, 1);
    if (st != NC_ST_OK)
    {
        b->status = st;
        return;
    }

    volatile struct NCJobStatus *s = (volatile struct NCJobStatus *)(uintptr_t)j->status_pa;
    s->done = 0;
    s->status = NC_ST_RUNNING;
    if (j->pending == 0)
        complete(j);            /* empty range: done at once */
    else
    {
        __atomic_store_n(&j->state, JOB_ACTIVE, __ATOMIC_SEQ_CST);
        __asm__ volatile("dsb ish\n sev" ::: "memory");
    }
    b->result = slot;
    b->status = NC_ST_OK;
}

static void do_wait(volatile struct NCBlock *b)
{
    uint32_t slot = b->arg0;
    struct hv_job *j;
    uint64_t t0 = counter(), t1, freq;

    __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(freq));
    b->result = 0xffffffff;
    if (slot >= NC_ASYNC_JOBS || ajob[slot].state == JOB_FREE)
    {
        b->status = NC_ST_BADJOB;
        return;
    }
    j = &ajob[slot];

    /* still running: help for a while (pieces should be short: the 68k is stopped meanwhile) */
    if (__atomic_load_n(&j->state, __ATOMIC_SEQ_CST) == JOB_ACTIVE)
        join(j, 0, t0 + freq / 2000);    /* 0.5 ms */

    t1 = counter();
    b->info[NC_CALLINFO_TICKS] = (uint32_t)(t1 - t0);
    b->info[NC_CALLINFO_FREQ] = (uint32_t)freq;

    if (__atomic_load_n(&j->state, __ATOMIC_SEQ_CST) != JOB_COMPLETE)
    {
        b->status = NC_ST_RUNNING;
        return;
    }
    while (__atomic_load_n(&j->busy, __ATOMIC_SEQ_CST))
        __asm__ volatile("yield");
    __asm__ volatile("dmb ish" ::: "memory");

    if (j->fault && j->fault != JOB_CANCELLED)
        report_fault(b, j->fault_info, j->fault_kind);
    else
    {
        b->info[NC_MPINFO_CORES] = j->cores;
        b->info[NC_MPINFO_HELPED] = j->helped;
        b->result = j->sum;
        b->status = j->fault ? NC_ST_CANCELLED : NC_ST_OK;
    }
    __atomic_store_n(&j->state, JOB_FREE, __ATOMIC_SEQ_CST);
}

/* NC_OP_CANCEL: no piece of the job starts any more. A cancelled job is a job with a "fault" of
   the value JOB_CANCELLED: run_pieces only counts the remaining pieces off, here on core 0 at once,
   so the job completes as soon as the pieces that run right now on the helpers are done. */
static void do_cancel(volatile struct NCBlock *b)
{
    uint32_t slot = b->arg0, none = 0;
    struct hv_job *j;

    b->result = 0xffffffff;
    if (slot >= NC_ASYNC_JOBS || ajob[slot].state == JOB_FREE)
    {
        b->status = NC_ST_BADJOB;
        return;
    }
    j = &ajob[slot];
    if (__atomic_load_n(&j->state, __ATOMIC_SEQ_CST) == JOB_ACTIVE
        && __atomic_compare_exchange_n(&j->fault, &none, JOB_CANCELLED, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
        join(j, 0, 0);
    b->result = 0;
    b->status = NC_ST_OK;
}

/* Does the time limit work right now? Runs hypervisor code that would spin for 4 x the limit
   under a 50 us limit: with the FIQ path in place (boot.S, hv_config.h) the timer stops it.
   The spin is bounded, so a missing FIQ path costs 200 us, not the machine. */
static uint32_t selftest_spin(uintptr_t deadline)
{
    while (counter() < deadline)
        ;
    return 0;
}

static uint32_t features(void)
{
    struct hv_cpu *c = &hv_cpu[core_id()];
    uint64_t freq, limit;

    __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(freq));
    limit = freq / 20000;                       /* 50 us */
    c->fault_flag = 0;
    nc_call_native((uintptr_t)selftest_spin, counter() + 4 * limit, limit);
    return c->fault_flag == FAULT_TIMEOUT ? NC_FEAT_TIMEOUT : 0;
}

/* ---- Statistics (hypervisor 8, NC_OP_STATS) ---------------------------------------------------

   The SoC temperature comes from the AVS monitor of the BCM2711 (as Linux's bcm2711_thermal:
   10 bit value, 410.04 - 0.487 * value degrees). The register is read like native code, under
   the fault handler: on a machine without it (another SoC, an emulator) the read may abort,
   and then the temperature is simply not known. */
#define AVS_TEMP_STATUS     0xfd5d2200UL
#define AVS_TEMP_VALID      ((1u << 16) | (1u << 10))

static uint32_t read_avs(uintptr_t unused)
{
    (void)unused;
    return __builtin_bswap32(*(volatile uint32_t *)AVS_TEMP_STATUS);    /* little-endian register */
}

static uint32_t temperature(void)
{
    struct hv_cpu *c = &hv_cpu[core_id()];
    uint64_t freq;
    uint32_t v;

    __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(freq));
    c->fault_flag = 0;
    v = nc_call_native((uintptr_t)read_avs, 0, freq / 1000);
    if (c->fault_flag)
    {
        c->fault_flag = 0;
        return 0;
    }
    if ((v & AVS_TEMP_VALID) != AVS_TEMP_VALID)
        return 0;
    v &= 0x3ff;
    return v < 842 ? 410040 - 487 * v : 0;      /* below 0 degrees: not plausible here */
}

static struct NCStats stats_buf;

static void do_stats(volatile struct NCBlock *b)
{
    struct NCStats *s = &stats_buf;
    uint32_t size = b->arg1 < sizeof(*s) ? b->arg1 : sizeof(*s);
    uint64_t dst = size ? m68k_range_to_pa(b->arg0, size) : 0, now, freq;

    if (!dst || (dst & 3) || (size & 3))
    {
        b->status = NC_ST_BADADDR;
        return;
    }
    __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(freq));
    now = counter();
    s->version = 1;
    s->freq = (uint32_t)freq;
    s->now_hi = now >> 32;
    s->now_lo = (uint32_t)now;
    s->temperature = temperature();
    s->jobs = 0;
    for (int i = 0; i < NC_ASYNC_JOBS; i++)
        if (__atomic_load_n(&ajob[i].state, __ATOMIC_SEQ_CST) != JOB_FREE)
            s->jobs++;
    s->faults = fault_count;
    s->log_pos = hv_log.head;
    for (int i = 0; i < 4; i++)
    {
        uint64_t busy = __atomic_load_n(&hv_cpu[i].busy, __ATOMIC_RELAXED);

        s->core[i].busy_hi = busy >> 32;
        s->core[i].busy_lo = (uint32_t)busy;
        s->core[i].calls = hv_cpu[i].calls;
        s->core[i].flags = i == 0 ? NC_CORE_NATIVE
                         : (helpers_ready >> i) & 1 ? NC_CORE_NATIVE | NC_CORE_HELPER : 0;
    }
    s->hv_version = NC_HV_VERSION;
    s->features = features();
    copy_words((void *)(uintptr_t)dst, s, size);
    b->result = size;
    b->status = NC_ST_OK;
}

static void do_block(uint64_t pa)
{
    volatile struct NCBlock *b = (volatile struct NCBlock *)(uintptr_t)pa;

    if (b->magic != NC_BLOCK_MAGIC)
        return;
    if (!restricted)
        restrict_native_map();

    b->magic = NC_BLOCK_DONE;
    b->version = NC_HV_VERSION;

    switch (b->op)
    {
        case NC_OP_PING:
            b->result = NC_PING_REPLY;
            b->info[NC_PINGINFO_HELPERS] = helpers_ready;
            for (int i = 1; i < 4; i++)
            {
                b->info[NC_PINGINFO_PIECES + i - 1] = hv_cpu[i].pieces;
                b->info[NC_PINGINFO_WFE + i - 1] = hv_cpu[i].wfe_traps;
            }
            b->info[NC_PINGINFO_FEATURES] = features();
            b->status = NC_ST_OK;
            break;

        case NC_OP_SYNC:
        {
            uint64_t start = m68k_range_to_pa(b->arg0, b->arg1);
            if (!start || b->arg1 == 0)
            {
                b->status = NC_ST_BADADDR;
                break;
            }
            sync_icache(start, b->arg1);
            b->status = NC_ST_OK;
            break;
        }

        case NC_OP_CALL:
            do_call(b);
            break;

        case NC_OP_RUNMP:
            do_runmp(b);
            break;

        case NC_OP_START:
            do_start(b);
            break;

        case NC_OP_WAIT:
            do_wait(b);
            break;

        case NC_OP_CANCEL:
            do_cancel(b);
            break;

        case NC_OP_STATS:
            do_stats(b);
            break;

        case NC_OP_LOGREAD:
        {
            uint32_t size = b->arg1, pos = b->arg2, head, lost = 0, n, i;
            uint64_t dst = size ? m68k_range_to_pa(b->arg0, size) : 0;

            if (!dst)
            {
                b->status = NC_ST_BADADDR;
                break;
            }
            while (__atomic_exchange_n(&hv_log.lock, 1, __ATOMIC_ACQUIRE))
                __asm__ volatile("yield");
            head = hv_log.head;
            if (head - pos > NC_LOG_SIZE)
            {
                /* overwritten already (or a position from before the Pi started again) */
                lost = head - pos - NC_LOG_SIZE;
                pos = head - NC_LOG_SIZE;
                if (head < NC_LOG_SIZE)
                {
                    lost = 0;
                    pos = 0;
                }
            }
            n = head - pos;
            if (n > size)
                n = size;
            for (i = 0; i < n; i++)
                ((volatile char *)(uintptr_t)dst)[i] = hv_log.buf[(pos + i) & (NC_LOG_SIZE - 1)];
            __atomic_store_n(&hv_log.lock, 0, __ATOMIC_RELEASE);
            b->result = n;
            b->info[NC_LOGINFO_POS] = pos + n;
            b->info[NC_LOGINFO_LOST] = lost;
            b->status = NC_ST_OK;
            break;
        }

        case NC_OP_FAULTINFO:
        {
            uint32_t size = b->arg1 < sizeof(fault_rec) ? b->arg1 : sizeof(fault_rec);
            uint64_t dst = size ? m68k_range_to_pa(b->arg0, size) : 0;

            if (!dst || (dst & 3) || (size & 3))
            {
                b->status = NC_ST_BADADDR;
                break;
            }
            while (__atomic_exchange_n(&fault_lock, 1, __ATOMIC_ACQUIRE))
                __asm__ volatile("yield");
            copy_words((void *)(uintptr_t)dst, &fault_rec, size);
            __atomic_store_n(&fault_lock, 0, __ATOMIC_RELEASE);
            b->result = size;
            b->status = NC_ST_OK;
            break;
        }

        case NC_OP_INFO:
            b->info[NC_INFO_TRAPS] = stat_traps;
            b->info[NC_INFO_CALLS] = stat_calls;
            b->info[NC_INFO_FAULTS] = stat_faults;
            b->info[NC_INFO_SKIPPED] = stat_skipped;
            b->info[NC_INFO_LASTESR_HI] = last_fault[0] >> 32;
            b->info[NC_INFO_LASTESR_LO] = (uint32_t)last_fault[0];
            b->info[NC_INFO_LASTFAR_HI] = last_fault[1] >> 32;
            b->info[NC_INFO_LASTFAR_LO] = (uint32_t)last_fault[1];
            b->status = NC_ST_OK;
#ifdef HV_DIAG
            diag_log();
#ifdef HV_EMU68LOG
            emu68_log_dump();
#endif
#endif
            break;

        default:
            b->status = NC_ST_BADOP;
            break;
    }
}

/* ---- Trap handler -----------------------------------------------------------------------------

   Called from hv_lower_sync with x0-x30 of EL1 saved in regs[].

   Core 0: HCR_EL2.TPC, EC 0x18 for DC CIVAC / DC CVAC / DC IVAC executed by Emu68. Each one is
   done here on the physical address, so Emu68 sees no difference. A DC CIVAC on a 68k call
   block additionally processes the block.

   Helper cores (HV_HELPER_MASK): first EC 0x18 for Emu68's PMCR_EL0 access (MDCR_EL2.TPM, see
   hv_config.h), then EC 0x01 for every "wfe" of their idle loop. */

#define EC_WFX          0x01
#define EC_SYSINSN      0x18
#define EC_IABT_LOW     0x20
#define EC_DABT_LOW     0x24

#ifdef HV_S2
/* ---- Stage 2 for EL1 (HV_S2) ------------------------------------------------------------------

   IPA = PA, all 64 GB of the 36-bit IPA space Normal WB in 1 GB blocks (the stricter of the two
   stages wins, so the device mappings of Emu68 stay device), except the 2 MB of the hypervisor:
   not mapped. An access of Emu68 (EL1, cores 0, 1, 3) to the hypervisor comes here as a stage 2
   fault: logged with the EL1 PC and the address, then the 2 MB are mapped for EL1 and the
   instruction runs again (diagnosis: who touches the hypervisor). */

static uint32_t s2_hv_index, s2_faults;

#define S2_BLOCK_NORMAL (1UL | (0xFUL << 2) | (3UL << 6) | (3UL << 8) | (1UL << 10))
#define S2_TABLE        3UL

static void s2_init(uint64_t base)
{
    uint32_t g = base >> 30;

    for (int i = 0; i < 512; i++)
        s2_l1[i] = i < 64 ? ((uint64_t)i * GB) | S2_BLOCK_NORMAL : 0;
    for (int i = 0; i < 512; i++)
        s2_l2[i] = ((uint64_t)g * GB + (uint64_t)i * MB2) | S2_BLOCK_NORMAL;
    s2_hv_index = (base - (uint64_t)g * GB) / MB2;
    s2_l2[s2_hv_index] = 0;
    s2_l1[g] = (uint64_t)(uintptr_t)s2_l2 | S2_TABLE;
}

/* Stage 2 on for this core (EL1 of Emu68 runs on it) */
static void s2_enable(void)
{
    uint64_t r;

    __asm__ volatile("msr VTCR_EL2, %0\n msr VTTBR_EL2, %1\n isb"
                     :: "r"((uint64_t)VTCR_EL2_VAL), "r"((uint64_t)(uintptr_t)s2_l1) : "memory");
    __asm__ volatile("mrs %0, HCR_EL2" : "=r"(r));
    __asm__ volatile("msr HCR_EL2, %0\n isb\n tlbi vmalls12e1\n dsb nsh\n isb" :: "r"(r | HCR_VM) : "memory");
}

static void s2_fault(uint32_t core, uint64_t esr, uint64_t elr)
{
    uint64_t far, hpfar;
    struct line l;

    __asm__ volatile("mrs %0, FAR_EL2" : "=r"(far));
    __asm__ volatile("mrs %0, HPFAR_EL2" : "=r"(hpfar));
    s2_faults++;

    l.n = 0;
    put_s(&l, "nc diag: EL1 touched the hypervisor, core ");
    put_hex(&l, core, 1);
    put_s(&l, (esr >> 26) == EC_IABT_LOW ? " fetch" : (esr & (1 << 6)) ? " write" : " read");
    put_s(&l, ", ESR ");
    put_hex(&l, esr, 8);
    put_s(&l, ", EL1 PC ");
    put_hex(&l, elr, 16);
    put_s(&l, ", VA ");
    put_hex(&l, far, 16);
    put_s(&l, ", IPA ");
    put_hex(&l, (hpfar >> 4) << 12 | (far & 0xfff), 12);
    put_s(&l, "\n");
    hv_log_write(l.b, l.n);

    /* map it for EL1 from now on and let the instruction run again (ELR unchanged) */
    *(volatile uint64_t *)(uintptr_t)(MAINT_ALIAS + (uintptr_t)&s2_l2[s2_hv_index]) =
        ((uint64_t)(uintptr_t)__hv_start) | S2_BLOCK_NORMAL;
    __asm__ volatile("dsb ishst\n tlbi vmalls12e1is\n dsb ish\n isb" ::: "memory");
    __asm__ volatile("msr ELR_EL2, %0" :: "r"(elr));
}
#endif

static void helper_sync(uint32_t core, uint64_t esr, uint64_t elr)
{
    uint64_t r;

#ifdef HV_S2
    if ((esr >> 26) == EC_DABT_LOW || (esr >> 26) == EC_IABT_LOW)
    {
        s2_fault(core, esr, elr);
        return;
    }
#endif
    switch (esr >> 26)
    {
        case EC_SYSINSN:
            /* Emu68's first PMU access: take the core's idle "wfe" from now on, stop trapping
               the PMU and let the instruction run again at EL1. Emu68 wrote CNTHCTL_EL2 = 3. */
            __asm__ volatile("mrs %0, MDCR_EL2" : "=r"(r));
            __asm__ volatile("msr MDCR_EL2, %0" :: "r"(r & ~(uint64_t)MDCR_TPM));
#ifdef HV_CAPTURE_CORE2
            if (!((HV_HELPER_MASK >> core) & 1))
            {
                /* the housekeeper (core 2): set up at EL2 by hv_secondary, but no helper and no
                   WFE trap; with HV_S2 stage 2 on */
#ifdef HV_S2
                s2_enable();
#endif
                __asm__ volatile("msr ELR_EL2, %0" :: "r"(elr));
                return;
            }
#endif
            __asm__ volatile("mrs %0, CNTHCTL_EL2" : "=r"(r));
            __asm__ volatile("msr CNTHCTL_EL2, %0" :: "r"(r | CNTHCTL_EVNT));
            __asm__ volatile("mrs %0, HCR_EL2" : "=r"(r));
            __asm__ volatile("msr HCR_EL2, %0\n isb" :: "r"(r | HCR_TWE | HCR_FMO));
#ifdef HV_S2
            s2_enable();
#endif
            hv_cpu[core].captured = 1;
            __atomic_fetch_or(&helpers_ready, 1u << core, __ATOMIC_SEQ_CST);
            __asm__ volatile("msr ELR_EL2, %0" :: "r"(elr));
            return;

        case EC_WFX:
            /* The core is idle: help, or wait here in its place, then skip its "wfe" */
            hv_cpu[core].wfe_traps++;
            if (!help(core))
            {
                __asm__ volatile("wfe");
                help(core);
            }
            __asm__ volatile("msr ELR_EL2, %0" :: "r"(elr + 4));
            return;

        default:
        {
            extern void hv_panic(void);
            hv_panic();
        }
    }
}

enum { DC_CIVAC, DC_CVAC, DC_IVAC, DC_OTHER };

static int decode_dc(uint32_t iss)
{
    uint32_t op2 = (iss >> 17) & 7, op1 = (iss >> 14) & 7, crn = (iss >> 10) & 15, crm = (iss >> 1) & 15;

    if (crn != 7 || op2 != 1 || (iss & 1))
        return DC_OTHER;
    if (op1 == 3 && crm == 14) return DC_CIVAC;
    if (op1 == 3 && crm == 10) return DC_CVAC;
    if (op1 == 0 && crm == 6)  return DC_IVAC;
    return DC_OTHER;
}

#ifdef HV_RESETSPY
/* ---- Diagnosis: what is the 68k doing when it executes RESET? ----------------------------------

   The RESET instruction of the 68k ends in ps_send_reset() of Emu68, which first switches off
   the GIC CPU interface (gic_local_disable: a write to GICC_CTLR). Stage 2 for core 0 with that
   one page (0xFF842000) not mapped: every access of Emu68 to it traps here and is done in its
   place (ESR_EL2 says which register, size and direction); a write to GICC_CTLR that clears the
   enable bit logs the state of the 68k first: Emu68 keeps it in fixed ARM registers (NOTES.md 2). */

#define GICC_PAGE       0xFF842000UL
#define S2_NORMAL       (1UL | (0xFUL << 2) | (3UL << 6) | (3UL << 8) | (1UL << 10))    /* block */
#define ESR_ISV         (1UL << 24)

void hv_get_vregs(uint8_t *out);        /* hv_entry.S: q19, q20, q21 (48 bytes) */

static void rs_init(void)
{
    for (int i = 0; i < 512; i++)
        rs_l1[i] = i < 64 ? ((uint64_t)i * GB) | S2_NORMAL : 0;
    for (int i = 0; i < 512; i++)
        rs_l2[i] = (3 * GB + (uint64_t)i * MB2) | S2_NORMAL;
    for (int i = 0; i < 512; i++)
        rs_l3[i] = ((GICC_PAGE & ~(MB2 - 1)) + (uint64_t)i * PAGE) | S2_NORMAL | 2;     /* page */
    rs_l3[(GICC_PAGE & (MB2 - 1)) / PAGE] = 0;
    rs_l2[((GICC_PAGE & ~(MB2 - 1)) - 3 * GB) / MB2] = (uint64_t)(uintptr_t)rs_l3 | 3;
    rs_l1[3] = (uint64_t)(uintptr_t)rs_l2 | 3;
}

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/* n longs of 68k memory at addr, as the 68k sees it; "--------" where it is not Pi RAM */
static void dump68k(const char *name, uint32_t addr, uint32_t n)
{
    struct line l;

    l.n = 0;
    put_s(&l, name);
    put_hex(&l, addr, 8);
    put_s(&l, ":");
    for (uint32_t i = 0; i < n; i++)
    {
        uint64_t pa = el1_to_pa((uint64_t)addr + 4 * i), last = el1_to_pa((uint64_t)addr + 4 * i + 3);

        put_s(&l, " ");
        if (pa == PA_INVALID || last != pa + 3 || pa >= PA_LIMIT || !gb_has_ram(pa))
            put_s(&l, "--------");
        else
        {
            const volatile uint8_t *p = (const volatile uint8_t *)(uintptr_t)(MAINT_ALIAS + pa);
            put_hex(&l, (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3], 8);
        }
        if ((i & 7) == 7 || i == n - 1)
        {
            put_s(&l, "\n");
            hv_log_write(l.b, l.n);
            l.n = 0;
            put_s(&l, "   ");
        }
    }
}

static uint32_t rs_count;

static void reset_spy(const uint64_t *regs, uint32_t value)
{
    uint8_t v[48];
    struct line l;
    uint32_t pc = (uint32_t)regs[18], a7 = (uint32_t)regs[29], usp, isp, msp;

    hv_get_vregs(v);
    /* big-endian store of a q register: s[3] first. v19: last PC, SR; v21: MSP, ISP, USP, CACR */
    usp = be32(v + 40);
    isp = be32(v + 36);
    msp = be32(v + 32);

    l.n = 0;
    put_s(&l, "nc reset ");
    put_dec(&l, ++rs_count);
    put_s(&l, ": GICC_CTLR <- ");
    put_hex(&l, value, 8);
    put_s(&l, ", 68k PC ");
    put_hex(&l, pc, 8);
    put_s(&l, " last PC ");
    put_hex(&l, be32(v), 8);
    put_s(&l, " SR ");
    put_hex(&l, (uint32_t)v[4] << 8 | v[5], 4);
    put_s(&l, " USP ");
    put_hex(&l, usp, 8);
    put_s(&l, " ISP ");
    put_hex(&l, isp, 8);
    put_s(&l, " MSP ");
    put_hex(&l, msp, 8);
    put_s(&l, "\n");
    hv_log_write(l.b, l.n);

    l.n = 0;
    put_s(&l, "  D0-D7");
    for (int i = 19; i <= 26; i++)
    {
        put_s(&l, " ");
        put_hex(&l, (uint32_t)regs[i], 8);
    }
    put_s(&l, "\n  A0-A7");
    for (int i = 13; i <= 17; i++)
    {
        put_s(&l, " ");
        put_hex(&l, (uint32_t)regs[i], 8);
    }
    for (int i = 27; i <= 29; i++)
    {
        put_s(&l, " ");
        put_hex(&l, (uint32_t)regs[i], 8);
    }
    put_s(&l, "\n");
    hv_log_write(l.b, l.n);

    dump68k("  code at ", (pc - 32) & ~3u, 16);
    dump68k("  A7 at ", a7 & ~3u, 24);
    if (usp != a7)
        dump68k("  USP at ", usp & ~3u, 32);
    if (isp != a7 && isp != usp)
        dump68k("  ISP at ", isp & ~3u, 16);
}

/* A stage 2 fault of core 0: do the access to the GIC CPU interface in the place of Emu68 */
static void rs_fault(uint64_t *regs, uint64_t esr, uint64_t elr)
{
    uint64_t far, hpfar, ipa;
    uint32_t sas = (esr >> 22) & 3, srt = (esr >> 16) & 31;

    __asm__ volatile("mrs %0, FAR_EL2" : "=r"(far));
    __asm__ volatile("mrs %0, HPFAR_EL2" : "=r"(hpfar));
    ipa = ((hpfar >> 4) << 12) | (far & 0xfff);

    if ((esr >> 26) != EC_DABT_LOW || !(esr & ESR_ISV) || (ipa & ~(PAGE - 1)) != GICC_PAGE)
    {
        /* not something to emulate: say so, map the page for EL1 and stop spying */
        struct line l;

        l.n = 0;
        put_s(&l, "nc reset spy: gave up, ESR ");
        put_hex(&l, esr, 8);
        put_s(&l, " IPA ");
        put_hex(&l, ipa, 9);
        put_s(&l, "\n");
        hv_log_write(l.b, l.n);
        *(volatile uint64_t *)(uintptr_t)(MAINT_ALIAS + (uintptr_t)&rs_l3[(GICC_PAGE & (MB2 - 1)) / PAGE]) =
            GICC_PAGE | S2_NORMAL | 2;
        __asm__ volatile("dsb ishst\n tlbi vmalls12e1is\n dsb ish\n isb" ::: "memory");
        __asm__ volatile("msr ELR_EL2, %0" :: "r"(elr));
        return;
    }

    rs_emulated++;
    if (esr & (1 << 6))
    {
        uint64_t v = srt == 31 ? 0 : regs[srt];

        if ((ipa & (PAGE - 1)) == 0)
        {
            rs_ctlr_writes++;
            rs_last_ctlr = __builtin_bswap32((uint32_t)v);
        }

        /* GICC_CTLR, as Emu68 wrote it: little-endian register, big-endian store */
        if ((ipa & (PAGE - 1)) == 0 && sas == 2 && !(__builtin_bswap32((uint32_t)v) & 1))
            reset_spy(regs, __builtin_bswap32((uint32_t)v));
        switch (sas)
        {
            case 0: *(volatile uint8_t *)(uintptr_t)ipa = (uint8_t)v; break;
            case 1: *(volatile uint16_t *)(uintptr_t)ipa = (uint16_t)v; break;
            case 2: *(volatile uint32_t *)(uintptr_t)ipa = (uint32_t)v; break;
            default: *(volatile uint64_t *)(uintptr_t)ipa = v; break;
        }
    }
    else
    {
        uint64_t v;

        switch (sas)
        {
            case 0: v = *(volatile uint8_t *)(uintptr_t)ipa; break;
            case 1: v = *(volatile uint16_t *)(uintptr_t)ipa; break;
            case 2: v = *(volatile uint32_t *)(uintptr_t)ipa; break;
            default: v = *(volatile uint64_t *)(uintptr_t)ipa; break;
        }
        if (srt != 31)
            regs[srt] = v;
    }
    __asm__ volatile("msr ELR_EL2, %0" :: "r"(elr + 4));
}
#endif

void hv_sync(uint64_t *regs)
{
    uint64_t esr, elr;

    __asm__ volatile("mrs %0, ESR_EL2" : "=r"(esr));
    __asm__ volatile("mrs %0, ELR_EL2" : "=r"(elr));

    uint32_t core = core_id();
    if (core != 0)
    {
        helper_sync(core, esr, elr);
        return;
    }

#ifdef HV_S2
    if ((esr >> 26) == EC_DABT_LOW || (esr >> 26) == EC_IABT_LOW)
    {
        s2_fault(core, esr, elr);
        return;
    }
#endif
#ifdef HV_RESETSPY
    if ((esr >> 26) == EC_DABT_LOW || (esr >> 26) == EC_IABT_LOW)
    {
        rs_fault(regs, esr, elr);
        return;
    }
#endif
    if ((esr >> 26) != EC_SYSINSN)
    {
        extern void hv_panic(void);
        hv_panic();
    }

    uint32_t iss = esr & 0x1ffffff;
    uint32_t rt = (iss >> 5) & 31;
    uint64_t va = (rt == 31) ? 0 : regs[rt];
    int op = decode_dc(iss);

    stat_traps++;

    uint64_t pa = el1_to_pa(va);
    if (pa != PA_INVALID && pa < PA_LIMIT && pa_in_alias(pa))
    {
        uint64_t alias = MAINT_ALIAS + pa;

        switch (op)
        {
            case DC_CIVAC: __asm__ volatile("dc civac, %0" :: "r"(alias) : "memory"); break;
            case DC_CVAC:  __asm__ volatile("dc cvac, %0"  :: "r"(alias) : "memory"); break;
            case DC_IVAC:  __asm__ volatile("dc ivac, %0"  :: "r"(alias) : "memory"); break;
            default: break;
        }
        __asm__ volatile("dsb sy" ::: "memory");

        /* A request only with the doorbell in D0/D1 (nc_abi.h): never because of data */
        if (op == DC_CIVAC
            && (uint32_t)regs[NC_EMU68_REG_D0] == NC_DOORBELL_D0
            && (uint32_t)regs[NC_EMU68_REG_D1] == NC_DOORBELL_D1
            && va < 0x100000000UL && (pa & (NC_BLOCK_SIZE - 1)) == 0 && pa_is_ram(pa))
        {
            do_block(pa);
        }
    }
    else
    {
        stat_skipped++;
    }

    __asm__ volatile("msr ELR_EL2, %0" :: "r"(elr + 4));
}
