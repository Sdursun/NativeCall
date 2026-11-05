/*
    Resident EL2 part of the nativecall armstub (C side).

    Built with -mgeneral-regs-only: while a trap is handled, Emu68's FP/SIMD registers
    hold live 68k state and must not be touched. nc_call_native() saves them before
    native code runs.
*/

#include <stdint.h>
#include "hv_config.h"
#include "nc_abi.h"

uint32_t nc_call_native(uintptr_t fn, uintptr_t params);

/* ---- Per core state ---------------------------------------------------------------------------

   TPIDR_EL2 of each core that runs native code points at its entry (hv_entry.S). Offsets are
   shared with the assembler (hv_config.h). */

struct hv_cpu
{
    uint64_t jmpbuf[12];            /* recovery point of nc_call_native */
    uint32_t in_native;
    uint32_t fault_flag;
    uint64_t fault_info[3];         /* ESR, FAR, ELR */
    uint32_t captured;              /* helper core: HCR_EL2.TWE is on */
    uint32_t pieces;                /* RUNMP pieces run */
    uint32_t wfe_traps;             /* helper core: idle "wfe" taken */
    uint32_t reserved;
    uint64_t pad[14];
    uint8_t  params[NC_MP_PARAMS_MAX] __attribute__((aligned(16)));
} __attribute__((aligned(64)));

_Static_assert(__builtin_offsetof(struct hv_cpu, jmpbuf) == CPU_JMPBUF, "CPU_JMPBUF");
_Static_assert(__builtin_offsetof(struct hv_cpu, in_native) == CPU_IN_NATIVE, "CPU_IN_NATIVE");
_Static_assert(__builtin_offsetof(struct hv_cpu, fault_flag) == CPU_FAULT_FLAG, "CPU_FAULT_FLAG");
_Static_assert(__builtin_offsetof(struct hv_cpu, fault_info) == CPU_FAULT_INFO, "CPU_FAULT_INFO");
_Static_assert(sizeof(struct hv_cpu) == CPU_SIZE, "CPU_SIZE");

struct hv_cpu hv_cpu[4];
/* EL2 stacks of the helper cores (core 0 uses __stack_top) */
uint8_t hv_stacks[4][HV_STACK_SIZE] __attribute__((aligned(16)));

static inline uint32_t core_id(void)
{
    uint64_t mpidr;
    __asm__ volatile("mrs %0, MPIDR_EL1" : "=r"(mpidr));
    return mpidr & 3;
}

static uint64_t last_fault[3];      /* last fault reported to the 68k, for NC_OP_INFO */

/* ---- EL2 translation tables -------------------------------------------------------------------

   Identity map, 4 KB granule, 34-bit VA (T0SZ = 30), starting at level 1:

     0x0_0000_0000 - 0x0_00FF_FFFF  invalid: 68k chip RAM / CIA / custom chips live on the Amiga
                                    bus, not in Pi RAM. Native code touching them faults (and is
                                    caught) instead of corrupting low Pi memory.
     0x0_0100_0000 - 0x0_FBFF_FFFF  Normal WB (Pi RAM, includes all 68k Fast RAM)
     0x0_FC00_0000 - 0x0_FFFF_FFFF  Device (BCM2711 peripherals, low-peripheral mode)
     0x1_0000_0000 - 0x1_FFFF_FFFF  Normal WB (RAM above 4 GB: this hypervisor, Emu68 kernel/JIT)

   Cache maintenance alias at MAINT_ALIAS + PA, 0 - 8 GB without the hole (Normal, XN; Device
   for the peripherals). Emu68's own DC operations must reach every address, including the low
   16 MB: it flushes its kernel image there before moving itself to the top of RAM
   (src/aarch64/start.c:1440). Skipping that flush broke the first boot on the A1200.

   Same attributes as Emu68's EL1 mappings (Normal WB, inner shareable), so EL1 and EL2 see
   the same cache lines. */

#define PTE_VALID       (1UL << 0)
#define PTE_TABLE       (1UL << 1)              /* level 1/2: next level table */
#define PTE_BLOCK       (0UL << 1)
#define PTE_ATTR(n)     ((uint64_t)(n) << 2)    /* MAIR_EL2 index */
#define PTE_SH_INNER    (3UL << 8)
#define PTE_AF          (1UL << 10)
#define PTE_XN          (1UL << 54)

#define ATTR_NORMAL     0                       /* MAIR attr0 = 0xff */
#define ATTR_DEVICE     1                       /* MAIR attr1 = 0x04, Device-nGnRE */

#define BLOCK_NORMAL    (PTE_VALID | PTE_BLOCK | PTE_ATTR(ATTR_NORMAL) | PTE_SH_INNER | PTE_AF)
#define BLOCK_DEVICE    (PTE_VALID | PTE_BLOCK | PTE_ATTR(ATTR_DEVICE) | PTE_AF | PTE_XN)

#define GB              0x40000000UL
#define MB2             0x200000UL

#define SCTLR_M         (1UL << 0)
#define SCTLR_A         (1UL << 1)
#define SCTLR_C         (1UL << 2)
#define SCTLR_I         (1UL << 12)
#define SCTLR_WXN       (1UL << 19)

#define MAINT_ALIAS     0x200000000UL           /* VA of PA 0 for cache maintenance */
#define PA_LIMIT        0x200000000UL           /* 8 GB */

uint64_t hv_l1[512]        __attribute__((aligned(4096)));  /* TTBR0_EL2 of all cores */
#define l1 hv_l1
static uint64_t l2_gb0[512] __attribute__((aligned(4096)));
static uint64_t l2_gb3[512] __attribute__((aligned(4096)));

/* Ranges that are Normal memory in the identity map, visible to native code */
static inline int pa_is_ram(uint64_t pa)
{
    return (pa >= 0x01000000UL && pa < 0xFC000000UL) || (pa >= 0x100000000UL && pa < PA_LIMIT);
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

void hv_init(void)
{
    led_flash(2);

    for (int i = 0; i < 512; i++)
    {
        uint64_t a0 = (uint64_t)i * MB2;
        uint64_t a3 = 3 * GB + (uint64_t)i * MB2;

        l2_gb0[i] = (a0 < 0x01000000UL) ? 0 : (a0 | BLOCK_NORMAL);
        l2_gb3[i] = a3 | ((a3 < 0xFC000000UL) ? BLOCK_NORMAL : BLOCK_DEVICE);
    }

    l1[0] = (uint64_t)(uintptr_t)l2_gb0 | PTE_VALID | PTE_TABLE;
    l1[1] = 1 * GB | BLOCK_NORMAL;
    l1[2] = 2 * GB | BLOCK_NORMAL;
    l1[3] = (uint64_t)(uintptr_t)l2_gb3 | PTE_VALID | PTE_TABLE;
    for (int i = 4; i < 8; i++)
        l1[i] = (uint64_t)i * GB | BLOCK_NORMAL;

    /* Maintenance alias: L1 entries 8-15 = PA 0-8 GB. GB 3 needs 2 MB blocks for the Device
       part, the existing l2_gb3 maps VA = PA there, so the alias gets its own table. */
    static uint64_t l2_alias_gb3[512] __attribute__((aligned(4096)));
    for (int i = 0; i < 512; i++)
        l2_alias_gb3[i] = l2_gb3[i] | PTE_XN;
    for (int i = 0; i < 8; i++)
    {
        if (i == 3)
            l1[8 + i] = (uint64_t)(uintptr_t)l2_alias_gb3 | PTE_VALID | PTE_TABLE;
        else
            l1[8 + i] = (uint64_t)i * GB | BLOCK_NORMAL | PTE_XN;
    }

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

/* ---- Requests --------------------------------------------------------------------------------- */

/* fn of module, params_size bytes at params (0: none): checked, physical addresses out.
   Returns NC_ST_OK or the error status. */
static uint32_t check_call(uint32_t fn, uint32_t params, uint32_t params_size, uint32_t module,
                           uint64_t *fn_pa, uint64_t *params_pa)
{
    uint64_t mod_pa = m68k_range_to_pa(module, 12);
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

static void report_fault(volatile struct NCBlock *b, const uint64_t *info)
{
    stat_faults++;
    for (int i = 0; i < 3; i++)
    {
        last_fault[i] = info[i];
        b->info[2 * i] = info[i] >> 32;
        b->info[2 * i + 1] = (uint32_t)info[i];
    }
    b->status = NC_ST_FAULT;
}

static void do_call(volatile struct NCBlock *b)
{
    struct hv_cpu *c = &hv_cpu[0];
    uint64_t fn_pa, params_pa, t0, t1, freq;

    stat_calls++;
    b->result = 0xffffffff;

    uint32_t st = check_call(b->arg0, b->arg1, 4, b->arg2, &fn_pa, &params_pa);
    if (st != NC_ST_OK)
    {
        b->status = st;
        return;
    }

    c->fault_flag = 0;
    __asm__ volatile("isb\n mrs %0, CNTPCT_EL0" : "=r"(t0));
    uint32_t r = nc_call_native(fn_pa, params_pa);
    __asm__ volatile("isb\n mrs %0, CNTPCT_EL0" : "=r"(t1));
    __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(freq));

    /* Native run time (the 68k cannot measure it: its interrupts wait meanwhile) */
    b->info[NC_CALLINFO_TICKS] = (uint32_t)(t1 - t0);
    b->info[NC_CALLINFO_FREQ] = (uint32_t)freq;

    if (c->fault_flag)
    {
        report_fault(b, c->fault_info);
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

static inline uint64_t counter(void)
{
    uint64_t t;
    __asm__ volatile("isb\n mrs %0, CNTPCT_EL0" : "=r"(t));
    return t;
}

/* The last piece of an async job is done: tell the 68k (status block, done word last) */
static void complete(struct hv_job *j)
{
    if (j->status_pa)
    {
        volatile struct NCJobStatus *s = (volatile struct NCJobStatus *)(uintptr_t)j->status_pa;

        s->result = j->sum;
        s->status = j->fault ? NC_ST_FAULT : NC_ST_OK;
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
            uint32_t r = nc_call_native(j->fn_pa, (uintptr_t)c->params);
            if (c->fault_flag)
            {
                uint32_t none = 0;
                if (__atomic_compare_exchange_n(&j->fault, &none, core + 1, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
                    for (int i = 0; i < 3; i++)
                        j->fault_info[i] = c->fault_info[i];
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
    uint64_t fn_pa, params_pa, status_pa = 0;
    uint32_t from = b->info[NC_MPINFO_FROM], to = b->info[NC_MPINFO_TO];
    uint32_t chunk = b->info[NC_MPINFO_CHUNK], size = b->info[NC_MPINFO_SIZE];

    if (size < 8 || size > NC_MP_PARAMS_MAX || (size & 3) || chunk == 0 || from > to || b->arg1 == 0)
        return NC_ST_BADADDR;
    uint32_t st = check_call(b->arg0, b->arg1, size, b->arg2, &fn_pa, &params_pa);
    if (st != NC_ST_OK)
        return st;
    if (async)
    {
        status_pa = m68k_range_to_pa(b->info[NC_JOBINFO_STATUS], sizeof(struct NCJobStatus));
        if (!status_pa || (status_pa & 3))
            return NC_ST_BADADDR;
    }

    copy_words(j->params, (volatile void *)(uintptr_t)params_pa, size);
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
        report_fault(b, job.fault_info);
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

    if (j->fault)
        report_fault(b, j->fault_info);
    else
    {
        b->info[NC_MPINFO_CORES] = j->cores;
        b->info[NC_MPINFO_HELPED] = j->helped;
        b->result = j->sum;
        b->status = NC_ST_OK;
    }
    __atomic_store_n(&j->state, JOB_FREE, __ATOMIC_SEQ_CST);
}

static void do_block(uint64_t pa)
{
    volatile struct NCBlock *b = (volatile struct NCBlock *)(uintptr_t)pa;

    if (b->magic != NC_BLOCK_MAGIC)
        return;

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

static void helper_sync(uint32_t core, uint64_t esr, uint64_t elr)
{
    uint64_t r;

    switch (esr >> 26)
    {
        case EC_SYSINSN:
            /* Emu68's first PMU access: take the core's idle "wfe" from now on, stop trapping
               the PMU and let the instruction run again at EL1. Emu68 wrote CNTHCTL_EL2 = 3. */
            __asm__ volatile("mrs %0, MDCR_EL2" : "=r"(r));
            __asm__ volatile("msr MDCR_EL2, %0" :: "r"(r & ~(uint64_t)MDCR_TPM));
            __asm__ volatile("mrs %0, CNTHCTL_EL2" : "=r"(r));
            __asm__ volatile("msr CNTHCTL_EL2, %0" :: "r"(r | CNTHCTL_EVNT));
            __asm__ volatile("mrs %0, HCR_EL2" : "=r"(r));
            __asm__ volatile("msr HCR_EL2, %0\n isb" :: "r"(r | HCR_TWE));
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
    if (pa != PA_INVALID && pa < PA_LIMIT)
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
