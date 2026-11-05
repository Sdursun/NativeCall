/*
    nativecall.library - 68k side of nativecall (NOTES.md 9, sdk/README.md).

    A request: fill the 64-byte call block, nc_trigger() runs CPUSHL DC,(A0) on it in
    supervisor mode with the doorbell in D0/D1. Emu68 turns that into "dc civac", the
    hypervisor traps it and answers in the block. Without the hypervisor the block's status
    stays NC_ST_NONE.

    One call block for all callers: a request runs under Forbid() and is short (the caller
    keeps native calls around 1 ms), so tasks simply take turns. The module list is protected
    by a semaphore. Built with VBCC, registerised entry points, no startup code.
*/

#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/memory.h>
#include <exec/libraries.h>
#include <exec/resident.h>
#include <exec/semaphores.h>
#include <exec/lists.h>
#include <devices/timer.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/timer.h>

#include "nc_abi.h"

#define LIB_VERSION     1
#define LIB_REVISION    5

/* Native code cannot see the low 16 MB (chip RAM, Z2 space, I/O): hypervisor/hv.c */
#define NC_LOW_LIMIT    0x01000000UL

/* NC_Run: aim for this much native time per call (us) */
#define RUN_TARGET_US   500

/* Per kernel entry: the piece size NC_Run settled on (so that a kernel run every frame does
   not start again at 1: the first ncdemo version spent more time on small calls than on the
   work) and the native run time statistics (hypervisor 2+). Least recently added is reused. */
#define KINFO_COUNT     32

struct KernelInfo
{
    APTR                 entry;
    ULONG                step[2];       /* piece size of NC_Run [0] and NC_RunMP [1] */
    ULONG                learned[2];    /* step comes from a whole run */
    struct NCKernelStats stats;
};

struct NCBase
{
    struct Library          lib;
    BPTR                    seglist;
    struct timerequest      tr;
    struct SignalSemaphore  sem;
    struct MinList          modules;
    volatile struct NCBlock *block;
    APTR                    block_alloc;
    LONG                    available;
    ULONG                   last_fault[3];
    ULONG                   eclock_freq;
    struct KernelInfo       kinfo[KINFO_COUNT];
    ULONG                   kinfo_next;
    BOOL                    hv_timing;      /* the hypervisor reports native run times */
    ULONG                   hv_version;     /* NC_HV_VERSION of the last reply */
    ULONG                   cores;          /* ARM cores for NC_RunMP: 1 + helpers */
    /* NC_StartMP jobs (hypervisor 4): slot n = hypervisor job n, handle n + 1 */
    volatile struct NCJobStatus *job_status;    /* NC_ASYNC_JOBS, Fast RAM, for the hypervisor */
    APTR                    job_status_alloc;
    struct NCModule        *job_module[NC_ASYNC_JOBS];   /* NULL: slot free */
    UBYTE                   job_block[NC_ASYNC_JOBS];    /* its entry in job_status[] */
};

struct NCModule
{
    struct MinNode  node;
    APTR            alloc;
    ULONG           alloc_size;
    UBYTE          *base;
    ULONG           size;
};

struct ExecBase *SysBase;
struct DosLibrary *DOSBase;
struct Device *TimerBase;

void nc_trigger(__reg("a0") volatile struct NCBlock *block);
ULONG LIB_NC_WaitJob(__reg("a6") struct NCBase *nb, __reg("d0") ULONG job, __reg("a0") ULONG *status);

const char LibName[] = "nativecall.library";
const char LibIdString[] = "nativecall.library 1.5 (02.11.2026)\r\n";
const char LibVersTag[] = "$VER: nativecall.library 1.5 (02.11.2026)";

/* ---- Memory ---- */

static APTR alloc_high(ULONG size)
{
    APTR p = AllocMem(size, MEMF_FAST | MEMF_PUBLIC | MEMF_CLEAR);
    struct MemHeader *mh;

    if (p && (ULONG)p >= NC_LOW_LIMIT)
        return p;
    if (p)
        FreeMem(p, size);

    /* The default pool is low (e.g. Z2 Fast RAM first): take it from a high Fast RAM header */
    p = NULL;
    Forbid();
    for (mh = (struct MemHeader *)SysBase->MemList.lh_Head; mh->mh_Node.ln_Succ; mh = (struct MemHeader *)mh->mh_Node.ln_Succ)
    {
        if ((mh->mh_Attributes & MEMF_FAST) && (ULONG)mh->mh_Lower >= NC_LOW_LIMIT)
        {
            p = Allocate(mh, size);
            if (p)
                break;
        }
    }
    Permit();

    if (p)
    {
        ULONG *q = p;
        ULONG n = (size + 3) >> 2;
        while (n--)
            *q++ = 0;
    }
    return p;
}

/* ---- Requests ---- */

/* in: NULL or 5 ULONGs for info[0..4] (NC_OP_RUNMP, NC_OP_START); info: NULL or 8 ULONGs out */
static ULONG request(struct NCBase *nb, ULONG op, ULONG a0, ULONG a1, ULONG a2, ULONG *status,
                     const ULONG *in, ULONG *info)
{
    volatile struct NCBlock *b = nb->block;
    ULONG result, st;
    int i;

    Forbid();
    b->op = op;
    b->arg0 = a0;
    b->arg1 = a1;
    b->arg2 = a2;
    b->result = 0xffffffff;
    b->status = NC_ST_NONE;
    if (in)
        for (i = 0; i < 5; i++)
            b->info[i] = in[i];
    b->info[NC_CALLINFO_TICKS] = 0;     /* hypervisor 1 does not set them */
    b->info[NC_CALLINFO_FREQ] = 0;
    b->magic = NC_BLOCK_MAGIC;

    nc_trigger(b);

    st = b->status;
    result = b->result;
    if (st != NC_ST_NONE)
        nb->hv_version = b->version;
    if (st == NC_ST_FAULT)
    {
        nb->last_fault[0] = b->info[1];
        nb->last_fault[1] = b->info[3];
        nb->last_fault[2] = b->info[5];
    }
    if (info)
        for (i = 0; i < 8; i++)
            info[i] = b->info[i];
    b->magic = 0;
    Permit();

    if (status)
        *status = st;
    return result;
}

static BOOL available(struct NCBase *nb)
{
    ULONG st;

    if (nb->available >= 0)
        return (BOOL)nb->available;

    Forbid();
    if (nb->available < 0)
    {
        nb->available = 0;

        /* CPUSHL exists on 68040/68060 only (Emu68 reports a 68040) */
        if (SysBase->AttnFlags & AFF_68040)
        {
            nb->block_alloc = alloc_high(2 * NC_BLOCK_SIZE);
            if (nb->block_alloc)
            {
                nb->block = (volatile struct NCBlock *)(((ULONG)nb->block_alloc + NC_BLOCK_SIZE - 1) & ~(ULONG)(NC_BLOCK_SIZE - 1));
                ULONG info[8], helpers;

                if (request(nb, NC_OP_PING, 0, 0, 0, &st, NULL, info) == NC_PING_REPLY && st == NC_ST_OK)
                {
                    nb->available = 1;
                    nb->cores = 1;
                    if (nb->hv_version >= 3)
                        for (helpers = info[NC_PINGINFO_HELPERS] & 0xe; helpers; helpers >>= 1)
                            nb->cores += helpers & 1;
                }
            }
        }
    }
    Permit();

    return (BOOL)nb->available;
}

/* ---- Library functions ---- */

BOOL LIB_NC_Available(__reg("a6") struct NCBase *nb)
{
    return available(nb);
}

static BOOL check_header(struct NativeHeader *h, ULONG size)
{
    ULONG i;

    if (size < 16 || h->magic != NARM_MAGIC || h->version != NARM_VERSION)
        return FALSE;
    if (h->entry_count == 0 || h->entry_count > NARM_MAX_ENTRIES || 12 + 4 * h->entry_count > size)
        return FALSE;
    for (i = 0; i < h->entry_count; i++)
        if (h->entries[i] >= size || (h->entries[i] & 15))
            return FALSE;
    return TRUE;
}

static void free_module(struct NCModule *m)
{
    FreeMem(m->alloc, m->alloc_size);
    FreeMem(m, sizeof(*m));
}

/* Copy a module image into Fast RAM above 16 MB, 4 KB aligned (the SDK builds with
   -mcmodel=tiny, so any 16-byte address works, but code using ADRP only works at its link
   address modulo 4 KB: NOTES.md 15), check it and make the ARM I-cache coherent.
   fill() writes the image to the destination, with ctx. */
static struct NCModule *load_module(struct NCBase *nb, ULONG size, BOOL (*fill)(UBYTE *dst, ULONG size, APTR ctx), APTR ctx)
{
    struct NCModule *m;
    ULONG st;

    if (!available(nb) || size < 16 || size > 0x100000)
        return NULL;

    m = AllocMem(sizeof(*m), MEMF_PUBLIC | MEMF_CLEAR);
    if (!m)
        return NULL;
    m->alloc_size = size + 4095;
    m->alloc = alloc_high(m->alloc_size);
    if (!m->alloc)
    {
        FreeMem(m, sizeof(*m));
        return NULL;
    }
    m->base = (UBYTE *)(((ULONG)m->alloc + 4095) & ~4095UL);
    m->size = size;

    if (!fill(m->base, size, ctx) || !check_header((struct NativeHeader *)m->base, size))
    {
        free_module(m);
        return NULL;
    }

    CacheClearU();

    /* The ARM I-cache does not see 68k cache operations (NOTES.md 6) */
    request(nb, NC_OP_SYNC, (ULONG)m->base, m->size, 0, &st, NULL, NULL);
    if (st != NC_ST_OK)
    {
        free_module(m);
        return NULL;
    }

    ObtainSemaphore(&nb->sem);
    AddTail((struct List *)&nb->modules, (struct Node *)&m->node);
    ReleaseSemaphore(&nb->sem);
    return m;
}

static BOOL fill_from_file(UBYTE *dst, ULONG size, APTR ctx)
{
    return (BOOL)(Read((BPTR)ctx, dst, size) == (LONG)size);
}

static BOOL fill_from_memory(UBYTE *dst, ULONG size, APTR ctx)
{
    CopyMem(ctx, dst, size);
    return TRUE;
}

APTR LIB_NC_LoadModule(__reg("a6") struct NCBase *nb, __reg("a0") CONST_STRPTR name)
{
    struct NCModule *m;
    BPTR fh;
    LONG size;

    if (!available(nb))
        return NULL;
    fh = Open(name, MODE_OLDFILE);
    if (!fh)
        return NULL;
    Seek(fh, 0, OFFSET_END);
    size = Seek(fh, 0, OFFSET_BEGINNING);
    m = size > 0 ? load_module(nb, (ULONG)size, fill_from_file, (APTR)fh) : NULL;
    Close(fh);
    return m;
}

APTR LIB_NC_LoadModuleMem(__reg("a6") struct NCBase *nb, __reg("a0") CONST_APTR image, __reg("d0") ULONG size)
{
    if (!image)
        return NULL;
    return load_module(nb, size, fill_from_memory, (APTR)image);
}

VOID LIB_NC_UnloadModule(__reg("a6") struct NCBase *nb, __reg("a0") APTR module)
{
    struct NCModule *m = module;

    int i;

    if (!m)
        return;

    /* jobs still running in this module's code: let them end first */
    for (i = 0; i < NC_ASYNC_JOBS; i++)
        if (nb->job_module[i] == m)
            LIB_NC_WaitJob(nb, i + 1, NULL);

    ObtainSemaphore(&nb->sem);
    Remove((struct Node *)&m->node);
    ReleaseSemaphore(&nb->sem);

    /* forget its kernels: a module loaded at the same address later would inherit their
       piece sizes and statistics */
    Forbid();
    for (i = 0; i < KINFO_COUNT; i++)
        if ((UBYTE *)nb->kinfo[i].entry >= m->base && (UBYTE *)nb->kinfo[i].entry < m->base + m->size)
            nb->kinfo[i].entry = NULL;
    Permit();

    free_module(m);
}

APTR LIB_NC_GetEntry(__reg("a6") struct NCBase *nb, __reg("a0") APTR module, __reg("d0") ULONG index)
{
    struct NCModule *m = module;
    struct NativeHeader *h;

    if (!m)
        return NULL;
    h = (struct NativeHeader *)m->base;
    if (index >= h->entry_count)
        return NULL;
    return m->base + h->entries[index];
}

/* Module containing entry, or NULL */
static struct NCModule *find_module(struct NCBase *nb, APTR entry)
{
    struct MinNode *n;
    struct NCModule *found = NULL;

    ObtainSemaphoreShared(&nb->sem);
    for (n = nb->modules.mlh_Head; n->mln_Succ; n = n->mln_Succ)
    {
        struct NCModule *m = (struct NCModule *)n;

        if ((UBYTE *)entry >= m->base && (UBYTE *)entry < m->base + m->size)
        {
            found = m;
            break;
        }
    }
    ReleaseSemaphore(&nb->sem);
    return found;
}

/* Kernel info of entry; create: take a slot if there is none. Call under Forbid(). */
static struct KernelInfo *kernel_info(struct NCBase *nb, APTR entry, BOOL create)
{
    struct KernelInfo *ki;
    int i;

    for (i = 0; i < KINFO_COUNT; i++)
        if (nb->kinfo[i].entry == entry)
            return &nb->kinfo[i];
    if (!create)
        return NULL;
    ki = &nb->kinfo[nb->kinfo_next];
    nb->kinfo_next = (nb->kinfo_next + 1) % KINFO_COUNT;
    ki->entry = entry;
    ki->step[0] = ki->step[1] = 1;
    ki->learned[0] = ki->learned[1] = 0;
    ki->stats.calls = ki->stats.total_us = ki->stats.max_us = ki->stats.last_us = 0;
    return ki;
}

/* One native call; *us = native run time (0xffffffff: the hypervisor does not measure).
   mp: NULL for NC_OP_CALL, or the 4 input words of NC_OP_RUNMP. */
static ULONG call(struct NCBase *nb, APTR entry, APTR params, struct NCModule *m, ULONG *status, ULONG *us,
                  const ULONG *mp)
{
    ULONG info[8], result, per_us;
    struct KernelInfo *ki;

    result = request(nb, mp ? NC_OP_RUNMP : NC_OP_CALL, (ULONG)entry, (ULONG)params, (ULONG)m->base,
                     status, mp, info);
    per_us = info[NC_CALLINFO_FREQ] / 1000000;      /* 54 on a Pi 4: no 64-bit division */
    if (per_us == 0)
    {
        *us = 0xffffffff;
        return result;
    }
    *us = info[NC_CALLINFO_TICKS] / per_us;

    Forbid();
    nb->hv_timing = TRUE;
    ki = kernel_info(nb, entry, TRUE);
    ki->stats.calls++;
    ki->stats.total_us += *us;
    ki->stats.last_us = *us;
    if (*us > ki->stats.max_us)
        ki->stats.max_us = *us;
    Permit();
    return result;
}

ULONG LIB_NC_CallStatus(__reg("a6") struct NCBase *nb, __reg("a0") APTR entry, __reg("a1") APTR params, __reg("a2") ULONG *status)
{
    struct NCModule *m;
    ULONG st, us, result;

    if (!available(nb))
    {
        if (status) *status = NC_ST_NONE;
        return 0xffffffff;
    }
    m = find_module(nb, entry);
    if (!m)
    {
        if (status) *status = NC_ST_BADENTRY;
        return 0xffffffff;
    }
    result = call(nb, entry, params, m, &st, &us, NULL);
    if (status)
        *status = st;
    return result;
}

ULONG LIB_NC_Call(__reg("a6") struct NCBase *nb, __reg("a0") APTR entry, __reg("a1") APTR params)
{
    return LIB_NC_CallStatus(nb, entry, params, NULL);
}

/*
    [from, to) in pieces; params begins with { ULONG from, to; }. A call should take about
    RUN_TARGET_US, measured by the hypervisor (version 2+) or, with an older one, around the
    call with timer.device's EClock.

    The cost of a piece often depends on where it is (the empty top of a screen, then the
    busy middle), so growing on cheap pieces runs into a long call later in the same run.
    Only the first run of a kernel grows its pieces as it goes; later runs keep the learned
    size and only shrink it. Between runs the size doubles when every piece was short and
    becomes the smallest one of the run when a piece was too long.
*/
static ULONG shrink(ULONG n, ULONG us)
{
    n = n > 0xffffffff / RUN_TARGET_US ? n / us * RUN_TARGET_US : n * RUN_TARGET_US / us;
    return n ? n : 1;
}

/* mp: 0 for NC_Run, else params_size for NC_RunMP (the hypervisor splits each call further) */
static ULONG run_range(struct NCBase *nb, APTR entry, APTR params, ULONG from, ULONG to, ULONG *status,
                       ULONG mp)
{
    ULONG *range = params;
    ULONG pos = from, step, n, sum = 0, st = NC_ST_OK, us, max_us = 0, in[5];
    BOOL learned, too_long = FALSE;
    struct NCModule *m;
    struct KernelInfo *ki;
    struct EClockVal t0, t1;
    BOOL eclock;
    int k = mp ? 1 : 0;

    Forbid();
    ki = kernel_info(nb, entry, FALSE);
    step = ki ? ki->step[k] : 1;
    learned = ki ? ki->learned[k] : FALSE;
    Permit();

    if (!available(nb))
        st = NC_ST_NONE;
    else if (!(m = find_module(nb, entry)))
        st = NC_ST_BADENTRY;
    else
    {
        while (pos < to)
        {
            n = to - pos < step ? to - pos : step;
            range[0] = pos;
            range[1] = pos + n;

            eclock = !nb->hv_timing;
            if (eclock)
                ReadEClock(&t0);
            if (mp)
            {
                /* about 4 pieces per core: an even share even if a helper starts late */
                in[NC_MPINFO_FROM] = pos;
                in[NC_MPINFO_TO] = pos + n;
                in[NC_MPINFO_CHUNK] = n / (4 * nb->cores);
                if (in[NC_MPINFO_CHUNK] == 0)
                    in[NC_MPINFO_CHUNK] = 1;
                in[NC_MPINFO_SIZE] = mp;
                in[NC_JOBINFO_STATUS] = 0;
                sum += call(nb, entry, params, m, &st, &us, in);
            }
            else
                sum += call(nb, entry, params, m, &st, &us, NULL);
            if (eclock)
            {
                ReadEClock(&t1);
                if (us == 0xffffffff)
                    us = (t1.ev_lo - t0.ev_lo) * 1000 / (nb->eclock_freq / 1000);
            }
            if (st != NC_ST_OK)
                break;
            pos += n;

            if (us == 0xffffffff)
                continue;
            if (us > max_us)
                max_us = us;
            if (us > RUN_TARGET_US * 3 / 2)
            {
                /* too long, also for a short last piece: shrink in proportion at once */
                too_long = TRUE;
                n = shrink(n, us);
                if (n < step)
                    step = n;
            }
            else if (!learned && n == step && us < RUN_TARGET_US / 2 && step < 0x40000000)
                step *= 2;      /* only full pieces say the size can grow */
        }

        /* a whole run of short pieces may take bigger ones next time */
        if (learned && !too_long && max_us < RUN_TARGET_US / 2 && pos == to &&
            to - from > step && step < 0x40000000)
            step *= 2;

        Forbid();
        ki = kernel_info(nb, entry, TRUE);
        ki->step[k] = step;
        ki->learned[k] = learned || pos == to;
        Permit();
    }

    if (status)
        *status = st;
    return sum;
}

ULONG LIB_NC_Run(__reg("a6") struct NCBase *nb, __reg("a0") APTR entry, __reg("a1") APTR params,
                 __reg("d0") ULONG from, __reg("d1") ULONG to, __reg("a2") ULONG *status)
{
    return run_range(nb, entry, params, from, to, status, 0);
}

ULONG LIB_NC_RunMP(__reg("a6") struct NCBase *nb, __reg("a0") APTR entry, __reg("a1") APTR params,
                   __reg("d0") ULONG from, __reg("d1") ULONG to, __reg("a2") ULONG *status,
                   __reg("d2") ULONG params_size)
{
    /* one core, an old hypervisor or params the hypervisor cannot copy: plain NC_Run */
    if (!available(nb) || nb->cores < 2 || nb->hv_version < 3 || params_size < 8
        || params_size > NC_MP_PARAMS_MAX || (params_size & 3))
        return run_range(nb, entry, params, from, to, status, 0);
    return run_range(nb, entry, params, from, to, status, params_size);
}

ULONG LIB_NC_Cores(__reg("a6") struct NCBase *nb)
{
    return available(nb) ? nb->cores : 0;
}

/* ---- Asynchronous jobs (hypervisor 4, nc_abi.h NC_OP_START) ---- */

static BOOL async_ready(struct NCBase *nb)
{
    if (!available(nb) || nb->hv_version < 4)
        return FALSE;
    if (!nb->job_status)
    {
        Forbid();
        if (!nb->job_status)
        {
            nb->job_status_alloc = alloc_high(NC_ASYNC_JOBS * sizeof(struct NCJobStatus) + 16);
            if (nb->job_status_alloc)
                nb->job_status = (volatile struct NCJobStatus *)(((ULONG)nb->job_status_alloc + 15) & ~15UL);
        }
        Permit();
    }
    return (BOOL)(nb->job_status != NULL);
}

/*
    Starts entry over [from, to) on the helper cores and returns at once: a handle (1..4) for
    NC_JobDone / NC_WaitJob, or 0 if the job could not start (no hypervisor 4, all slots taken,
    bad arguments) - then run it with NC_RunMP. chunk: units per piece, 0 = the piece size
    NC_Run learned for this kernel (about 0.5 ms), or 1/16 of the range.
*/
ULONG LIB_NC_StartMP(__reg("a6") struct NCBase *nb, __reg("a0") APTR entry, __reg("a1") APTR params,
                     __reg("d0") ULONG from, __reg("d1") ULONG to, __reg("d2") ULONG params_size,
                     __reg("d3") ULONG chunk)
{
    struct NCModule *m;
    struct KernelInfo *ki;
    ULONG in[5], st, slot;

    if (!async_ready(nb) || from > to || !(m = find_module(nb, entry)))
        return 0;
    if (!chunk)
    {
        Forbid();
        ki = kernel_info(nb, entry, FALSE);
        chunk = ki && ki->learned[0] ? ki->step[0] : (to - from) / 16;
        Permit();
        if (!chunk)
            chunk = 1;
    }
    in[NC_MPINFO_FROM] = from;
    in[NC_MPINFO_TO] = to;
    in[NC_MPINFO_CHUNK] = chunk;
    in[NC_MPINFO_SIZE] = params_size;

    Forbid();
    /* the hypervisor takes the first free slot: tell it which status block belongs to it */
    for (slot = 0; slot < NC_ASYNC_JOBS && nb->job_module[slot]; slot++)
        ;
    if (slot == NC_ASYNC_JOBS)
    {
        Permit();
        return 0;
    }
    in[NC_JOBINFO_STATUS] = (ULONG)&nb->job_status[slot];
    slot = request(nb, NC_OP_START, (ULONG)entry, (ULONG)params, (ULONG)m->base, &st, in, NULL);
    if (st == NC_ST_OK && slot < NC_ASYNC_JOBS)
    {
        /* the hypervisor's slot and ours agree (both free a slot on WAIT); keep the link anyway */
        nb->job_module[slot] = m;
        nb->job_block[slot] = (UBYTE)(((ULONG)in[NC_JOBINFO_STATUS] - (ULONG)nb->job_status) / sizeof(struct NCJobStatus));
    }
    Permit();

    return st == NC_ST_OK && slot < NC_ASYNC_JOBS ? slot + 1 : 0;
}

/* TRUE when the job is complete (reads memory only, no request) */
BOOL LIB_NC_JobDone(__reg("a6") struct NCBase *nb, __reg("d0") ULONG job)
{
    if (job < 1 || job > NC_ASYNC_JOBS || !nb->job_status || !nb->job_module[job - 1])
        return TRUE;
    return (BOOL)(nb->job_status[nb->job_block[job - 1]].done != 0);
}

/* Waits for the job (core 0 helps in steps of at most 0.5 ms, the 68k runs in between) and
   returns the sum of its results. *status (may be NULL): NC_ST_OK, NC_ST_FAULT, NC_ST_BADJOB.
   The job's output may be read after this, not before. */
ULONG LIB_NC_WaitJob(__reg("a6") struct NCBase *nb, __reg("d0") ULONG job, __reg("a0") ULONG *status)
{
    ULONG st = NC_ST_BADJOB, result = 0xffffffff;

    if (job >= 1 && job <= NC_ASYNC_JOBS && nb->job_module[job - 1])
    {
        do
            result = request(nb, NC_OP_WAIT, job - 1, 0, 0, &st, NULL, NULL);
        while (st == NC_ST_RUNNING);
        nb->job_module[job - 1] = NULL;
    }
    if (status)
        *status = st;
    return result;
}

BOOL LIB_NC_GetKernelStats(__reg("a6") struct NCBase *nb, __reg("a0") APTR entry,
                           __reg("a1") struct NCKernelStats *stats, __reg("d0") BOOL reset)
{
    struct KernelInfo *ki;

    Forbid();
    ki = kernel_info(nb, entry, FALSE);
    if (ki && stats)
        *stats = ki->stats;
    else if (stats)
        stats->calls = stats->total_us = stats->max_us = stats->last_us = 0;
    if (ki && reset)
        ki->stats.calls = ki->stats.total_us = ki->stats.max_us = ki->stats.last_us = 0;
    Permit();
    return (BOOL)(ki != NULL && nb->hv_timing);
}

APTR LIB_NC_AllocMem(__reg("a6") struct NCBase *nb, __reg("d0") ULONG size)
{
    return alloc_high(size);
}

VOID LIB_NC_FreeMem(__reg("a6") struct NCBase *nb, __reg("a1") APTR memory, __reg("d0") ULONG size)
{
    if (memory)
        FreeMem(memory, size);
}

BOOL LIB_NC_GetInfo(__reg("a6") struct NCBase *nb, __reg("a0") ULONG *info)
{
    ULONG st;

    if (!available(nb))
        return FALSE;
    request(nb, NC_OP_INFO, 0, 0, 0, &st, NULL, info);
    return (BOOL)(st == NC_ST_OK);
}

VOID LIB_NC_LastFault(__reg("a6") struct NCBase *nb, __reg("a0") ULONG *fault)
{
    fault[0] = nb->last_fault[0];
    fault[1] = nb->last_fault[1];
    fault[2] = nb->last_fault[2];
}

/* ---- Library management ---- */

static void cleanup(struct NCBase *nb)
{
    struct MinNode *n;
    int i;

    while ((n = nb->modules.mlh_Head)->mln_Succ)
    {
        Remove((struct Node *)n);
        free_module((struct NCModule *)n);
    }
    for (i = 0; i < NC_ASYNC_JOBS; i++)
        if (nb->job_module[i])
            LIB_NC_WaitJob(nb, i + 1, NULL);
    if (nb->job_status_alloc)
        FreeMem(nb->job_status_alloc, NC_ASYNC_JOBS * sizeof(struct NCJobStatus) + 16);
    if (nb->block_alloc)
        FreeMem(nb->block_alloc, 2 * NC_BLOCK_SIZE);
    if (TimerBase)
        CloseDevice((struct IORequest *)&nb->tr);
    if (DOSBase)
        CloseLibrary((struct Library *)DOSBase);
}

struct NCBase *LibInit(__reg("d0") struct NCBase *nb, __reg("a0") BPTR seglist, __reg("a6") struct ExecBase *sysbase)
{
    struct EClockVal ev;

    SysBase = sysbase;
    nb->seglist = seglist;
    nb->lib.lib_Node.ln_Type = NT_LIBRARY;
    nb->lib.lib_Node.ln_Name = (char *)LibName;
    nb->lib.lib_Flags = LIBF_SUMUSED | LIBF_CHANGED;
    nb->lib.lib_Version = LIB_VERSION;
    nb->lib.lib_Revision = LIB_REVISION;
    nb->lib.lib_IdString = (APTR)LibIdString;
    nb->available = -1;
    InitSemaphore(&nb->sem);
    nb->modules.mlh_Head = (struct MinNode *)&nb->modules.mlh_Tail;
    nb->modules.mlh_Tail = NULL;
    nb->modules.mlh_TailPred = (struct MinNode *)&nb->modules.mlh_Head;

    DOSBase = (struct DosLibrary *)OpenLibrary("dos.library", 39);
    if (DOSBase && OpenDevice(TIMERNAME, UNIT_MICROHZ, (struct IORequest *)&nb->tr, 0) == 0)
    {
        TimerBase = nb->tr.tr_node.io_Device;
        nb->eclock_freq = ReadEClock(&ev);
        return nb;
    }

    cleanup(nb);
    FreeMem((UBYTE *)nb - nb->lib.lib_NegSize, nb->lib.lib_NegSize + nb->lib.lib_PosSize);
    return NULL;
}

struct NCBase *LibOpen(__reg("a6") struct NCBase *nb)
{
    nb->lib.lib_OpenCnt++;
    nb->lib.lib_Flags &= ~LIBF_DELEXP;
    return nb;
}

BPTR LibExpunge(__reg("a6") struct NCBase *nb)
{
    BPTR seglist;

    if (nb->lib.lib_OpenCnt)
    {
        nb->lib.lib_Flags |= LIBF_DELEXP;
        return 0;
    }
    seglist = nb->seglist;
    Remove((struct Node *)nb);
    cleanup(nb);
    FreeMem((UBYTE *)nb - nb->lib.lib_NegSize, nb->lib.lib_NegSize + nb->lib.lib_PosSize);
    return seglist;
}

BPTR LibClose(__reg("a6") struct NCBase *nb)
{
    if (--nb->lib.lib_OpenCnt == 0 && (nb->lib.lib_Flags & LIBF_DELEXP))
        return LibExpunge(nb);
    return 0;
}

ULONG LibNull(void)
{
    return 0;
}

/* Order = LVOs of sfd/nativecall_lib.sfd (bias 30) */
const APTR FuncTable[] =
{
    (APTR)LibOpen,
    (APTR)LibClose,
    (APTR)LibExpunge,
    (APTR)LibNull,
    (APTR)LIB_NC_Available,
    (APTR)LIB_NC_LoadModule,
    (APTR)LIB_NC_UnloadModule,
    (APTR)LIB_NC_GetEntry,
    (APTR)LIB_NC_Call,
    (APTR)LIB_NC_CallStatus,
    (APTR)LIB_NC_Run,
    (APTR)LIB_NC_AllocMem,
    (APTR)LIB_NC_FreeMem,
    (APTR)LIB_NC_GetInfo,
    (APTR)LIB_NC_LastFault,
    (APTR)LIB_NC_LoadModuleMem,
    (APTR)LIB_NC_GetKernelStats,
    (APTR)LIB_NC_RunMP,
    (APTR)LIB_NC_Cores,
    (APTR)LIB_NC_StartMP,
    (APTR)LIB_NC_JobDone,
    (APTR)LIB_NC_WaitJob,
    (APTR)-1
};

const ULONG InitTable[4] =
{
    sizeof(struct NCBase),
    (ULONG)FuncTable,
    0,
    (ULONG)LibInit
};
