/*
    ncsupport.c - nativecall/nativecall.h for 68k programs. Compiled into every program by
    mk/nativecall.mk with the program's compiler (VBCC or GCC).
*/

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/execbase.h>
#include <devices/timer.h>
#include <dos/dos.h>
#include <dos/var.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/timer.h>
#include <proto/nativecall.h>
#include <string.h>
#ifndef NC_NO_ATEXIT
#include <stdlib.h>
#endif

#include <nativecall/nativecall.h>

struct Library *NativeCallBase;
static ULONG lib_users;

struct NCKernels
{
    struct NCKernels *next;         /* open sets of this program (closed at exit) */
    const struct NCKernelSet *set;
    APTR module;                    /* NULL: 68k only */
    ULONG status;
    APTR entry[1];                  /* set->count entries */
};

/* Kernel sets the program has open. A program that ends without nc_close() would leave its
   module loaded and its jobs running on the helper cores, writing memory that is free again:
   the first nc_open() registers nc__at_exit(), which closes what is still open (nc_close cancels
   the jobs). Not for programs without a C runtime: compile ncsupport.c with -DNC_NO_ATEXIT. */
static struct NCKernels *open_sets;

#ifndef NC_NO_ATEXIT
static void nc__at_exit(void)
{
    while (open_sets)
        nc_close(open_sets);
}
#endif

static ULONG kernels_size(const struct NCKernelSet *set)
{
    return sizeof(struct NCKernels) + (set->count ? set->count - 1 : 0) * sizeof(APTR);
}

/* CRC32 as zlib (tools/narm2c.py writes the build ID with zlib.crc32) */
static ULONG crc32(const UBYTE *p, ULONG n)
{
    ULONG crc = 0xffffffffUL;
    int k;

    while (n--)
    {
        crc ^= *p++;
        for (k = 0; k < 8; k++)
            crc = (crc & 1) ? 0xedb88320UL ^ (crc >> 1) : crc >> 1;
    }
    return ~crc;
}

static BOOL lib_has_mem_load(void)
{
    return NativeCallBase->lib_Version > 1 || NativeCallBase->lib_Revision >= 2;
}

/* Load a module file. expect_id != 0: only if its CRC32 matches (needs library 1.2, which
   loads from memory; 1.1 loads the file as it is). */
static APTR load_file(CONST_STRPTR path, ULONG expect_id, ULONG *status)
{
    BPTR fh;
    LONG size;
    UBYTE *buf;
    APTR m = NULL;

    if (!lib_has_mem_load())
        return NC_LoadModule(path);

    fh = Open(path, MODE_OLDFILE);
    if (!fh)
    {
        *status = NC_ST_NOMODULE;
        return NULL;
    }
    Seek(fh, 0, OFFSET_END);
    size = Seek(fh, 0, OFFSET_BEGINNING);
    buf = size > 0 ? AllocMem(size, MEMF_ANY) : NULL;
    if (buf && Read(fh, buf, size) == size)
    {
        if (expect_id && crc32(buf, size) != expect_id)
            *status = NC_ST_MISMATCH;
        else
            m = NC_LoadModuleMem(buf, size);
    }
    if (buf)
        FreeMem(buf, size);
    Close(fh);
    return m;
}

struct NCKernels *nc_open_path(const struct NCKernelSet *set, CONST_STRPTR path)
{
    struct NCKernels *k;
    char file[256];
    ULONG i, status = NC_ST_NOMODULE;

    k = AllocMem(kernels_size(set), MEMF_PUBLIC | MEMF_CLEAR);
    if (!k)
        return NULL;
    k->set = set;
    k->status = NC_ST_NONE;
#ifndef NC_NO_ATEXIT
    {
        static BOOL hooked;

        if (!hooked)
        {
            atexit(nc__at_exit);
            hooked = TRUE;
        }
    }
#endif
    k->next = open_sets;
    open_sets = k;

    if (!NativeCallBase)
        NativeCallBase = OpenLibrary((CONST_STRPTR)"nativecall.library", 1);
    if (!NativeCallBase)
        return k;
    lib_users++;

    if (!NC_Available())
        return k;

    if (path)
        /* explicit file: no version check */
        k->module = load_file(path, 0, &status);
    else if (GetVar((CONST_STRPTR)"NativeCall/ModuleDir", (STRPTR)file, sizeof(file), 0) > 0
             && AddPart((STRPTR)file, (CONST_STRPTR)set->file, sizeof(file)))
        /* development override: ENV:NativeCall/ModuleDir, no version check */
        k->module = load_file((CONST_STRPTR)file, 0, &status);
    else if (set->image && set->image_size && *set->image_size && lib_has_mem_load())
        /* the copy embedded in the program: always the right version */
        k->module = NC_LoadModuleMem(set->image, *set->image_size);
    else
    {
        strcpy(file, "PROGDIR:");
        if (strlen(set->file) < sizeof(file) - 9)
        {
            strcat(file, set->file);
            k->module = load_file((CONST_STRPTR)file, set->build_id ? *set->build_id : 0, &status);
        }
    }
    if (!k->module)
    {
        k->status = status;
        return k;
    }

    for (i = 0; i < set->count; i++)
    {
        k->entry[i] = NC_GetEntry(k->module, i);
        if (!k->entry[i])
        {
            /* module older than the program: fewer kernels than expected */
            NC_UnloadModule(k->module);
            k->module = NULL;
            k->status = NC_ST_MISMATCH;
            return k;
        }
    }
    k->status = NC_ST_OK;
    return k;
}

struct NCKernels *nc_open(const struct NCKernelSet *set)
{
    return nc_open_path(set, NULL);
}

void nc_close(struct NCKernels *k)
{
    struct NCKernels **link;

    if (!k)
        return;
    for (link = &open_sets; *link; link = &(*link)->next)
        if (*link == k)
        {
            *link = k->next;
            break;
        }
    if (k->module)
        NC_UnloadModule(k->module);
    if (NativeCallBase && --lib_users == 0)
    {
        CloseLibrary(NativeCallBase);
        NativeCallBase = NULL;
    }
    FreeMem(k, kernels_size(k->set));
}

BOOL nc_is_native(struct NCKernels *k)
{
    return k && k->module != NULL;
}

NC_U32 nc_call_68k(struct NCKernels *k, ULONG index, APTR params)
{
    return k->set->fallback[index](params);
}

NC_U32 nc_run_68k(struct NCKernels *k, ULONG index, APTR params, ULONG from, ULONG to)
{
    struct NCRange *r = params;

    r->from = from;
    r->to = to;
    return k->set->fallback[index](params);
}

NC_U32 nc_call(struct NCKernels *k, ULONG index, APTR params)
{
    ULONG status;
    NC_U32 result;

    if (!k->module)
        return nc_call_68k(k, index, params);
    result = NC_CallStatus(k->entry[index], params, &status);
    k->status = status;
    return result;
}

NC_U32 nc_run(struct NCKernels *k, ULONG index, APTR params, ULONG from, ULONG to)
{
    ULONG status;
    NC_U32 result;

    if (!k->module)
        return nc_run_68k(k, index, params, from, to);
    result = NC_Run(k->entry[index], params, from, to, &status);
    k->status = status;
    return result;
}

static BOOL lib_has_mp(void)
{
    return NativeCallBase->lib_Version > 1 || NativeCallBase->lib_Revision >= 4;
}

NC_U32 nc_run_mp(struct NCKernels *k, ULONG index, APTR params, ULONG params_size, ULONG from, ULONG to)
{
    ULONG status;
    NC_U32 result;

    if (!k->module)
        return nc_run_68k(k, index, params, from, to);
    if (!lib_has_mp())
        return nc_run(k, index, params, from, to);
    result = NC_RunMP(k->entry[index], params, from, to, &status, params_size);
    k->status = status;
    return result;
}

ULONG nc_cores(struct NCKernels *k)
{
    if (!k->module)
        return 0;
    return lib_has_mp() ? NC_Cores() : 1;
}

/* ---- Asynchronous jobs (library 1.5, hypervisor 4). nc_close() needs nothing extra:
   NC_UnloadModule waits for the module's jobs. ---- */

static BOOL lib_has_async(void)
{
    return NativeCallBase->lib_Version > 1 || NativeCallBase->lib_Revision >= 5;
}

BOOL nc_start_mp(struct NCKernels *k, struct NCJob *job, ULONG index, APTR params, ULONG params_size,
                 ULONG from, ULONG to)
{
    job->handle = 0;
    if (k->module && lib_has_async())
        job->handle = NC_StartMP(k->entry[index], params, from, to, params_size, 0);
    if (job->handle)
    {
        job->status = NC_ST_RUNNING;
        return TRUE;
    }

    /* could not start: do it now */
    job->result = nc_run_mp(k, index, params, params_size, from, to);
    job->status = k->module ? k->status : NC_ST_OK;
    return FALSE;
}

BOOL nc_done(struct NCKernels *k, struct NCJob *job)
{
    return k == NULL || job->handle == 0 || NC_JobDone(job->handle);
}

NC_U32 nc_wait(struct NCKernels *k, struct NCJob *job)
{
    if (job->handle)
    {
        job->result = NC_WaitJob(job->handle, &job->status);
        job->handle = 0;
    }
    k->status = job->status;
    return job->result;
}

/* Cancel (library 1.6): with an older library it waits */
NC_U32 nc_cancel(struct NCKernels *k, struct NCJob *job)
{
    if (job->handle && (NativeCallBase->lib_Version > 1 || NativeCallBase->lib_Revision >= 6))
    {
        job->result = NC_CancelJob(job->handle, &job->status);
        job->handle = 0;
    }
    return nc_wait(k, job);
}

ULONG nc_status(struct NCKernels *k)
{
    return k->status;
}

/* Time limit (library 1.6, hypervisor 6): older libraries have no NC_SetTimeout */
BOOL nc_set_timeout(struct NCKernels *k, ULONG microseconds)
{
    if (!k->module || !(NativeCallBase->lib_Version > 1 || NativeCallBase->lib_Revision >= 6))
        return FALSE;
    return (BOOL)(NC_SetTimeout(k->module, microseconds) != 0xffffffffUL);
}

BOOL nc_stats(struct NCKernels *k, ULONG index, struct NCKernelStats *stats, BOOL reset)
{
    stats->calls = stats->total_us = stats->max_us = stats->last_us = 0;
    if (!k->module || !lib_has_mem_load())      /* NC_GetKernelStats: library 1.2 */
        return FALSE;
    return NC_GetKernelStats(k->entry[index], stats, reset);
}

/* Fast RAM regions native code can see (hypervisor/hv.c: from 16 MB up), read once */
#define NC_REGIONS 8
static ULONG region_lo[NC_REGIONS], region_hi[NC_REGIONS], regions = 0xffffffffUL;

static void read_regions(void)
{
    struct MemHeader *mh;
    ULONG n = 0;

    Forbid();
    for (mh = (struct MemHeader *)SysBase->MemList.lh_Head; mh->mh_Node.ln_Succ && n < NC_REGIONS;
         mh = (struct MemHeader *)mh->mh_Node.ln_Succ)
    {
        ULONG lo = (ULONG)mh->mh_Lower, hi = (ULONG)mh->mh_Upper;

        if (!(mh->mh_Attributes & MEMF_FAST) || hi <= 0x01000000UL)
            continue;
        region_lo[n] = lo < 0x01000000UL ? 0x01000000UL : lo;
        region_hi[n] = hi;
        n++;
    }
    regions = n;
    Permit();
}

BOOL nc_addr_ok(CONST_APTR memory, ULONG size)
{
    ULONG start = (ULONG)memory, end = start + size, i;

    if (end < start)
        return FALSE;
    if (regions == 0xffffffffUL)
        read_regions();
    for (i = 0; i < regions; i++)
        if (start >= region_lo[i] && end <= region_hi[i])
            return TRUE;
    return FALSE;
}

BOOL nc_addr_limits(ULONG *lo, ULONG *hi)
{
    ULONG i, best = 0xffffffffUL;

    if (regions == 0xffffffffUL)
        read_regions();
    for (i = 0; i < regions; i++)
        if (best == 0xffffffffUL || region_hi[i] - region_lo[i] > region_hi[best] - region_lo[best])
            best = i;
    *lo = best == 0xffffffffUL ? 0 : region_lo[best];
    *hi = best == 0xffffffffUL ? 0 : region_hi[best];
    return (BOOL)(best != 0xffffffffUL);
}

/* ---- Clock ----

   Emu68 (detected by its devicetree.resource) gives the ARM counter to supervisor code through
   MOVEC: 0x0e0 CNTFRQ, 0x0e1 counter low, 0x0e2 counter high (Emu68 src/M68k_LINE4.c). A small
   routine run with Supervisor() reads high, low, high and the frequency into clock_out[]. It
   is built once at run time because it stores to absolute addresses. */

static UWORD clock_code[26];
static volatile ULONG clock_out[4];     /* high, low, high again, frequency */
static int clock_mode;                  /* 0: not set up, 1: Emu68, 2: timer.device */
static struct Device *clock_timer;

static UWORD *clock_store(UWORD *c, UWORD reg_word, volatile ULONG *dst)
{
    ULONG a = (ULONG)dst;

    *c++ = 0x4e7a;                      /* MOVEC <ctrl>,Dn */
    *c++ = reg_word;
    *c++ = (UWORD)(0x23c0 | (reg_word >> 12));      /* MOVE.L Dn,(xxx).L */
    *c++ = (UWORD)(a >> 16);
    *c++ = (UWORD)a;
    return c;
}

static void clock_setup(void)
{
    if (OpenResource((CONST_STRPTR)"devicetree.resource"))
    {
        UWORD *c = clock_code;

        c = clock_store(c, 0x10e2, &clock_out[0]);  /* D1 <- counter high */
        c = clock_store(c, 0x00e1, &clock_out[1]);  /* D0 <- counter low */
        c = clock_store(c, 0x10e2, &clock_out[2]);
        c = clock_store(c, 0x00e0, &clock_out[3]);  /* D0 <- frequency */
        *c = 0x4e73;                                /* RTE */
        CacheClearU();
        clock_mode = 1;
        return;
    }
    Forbid();
    clock_timer = (struct Device *)FindName(&SysBase->DeviceList, (CONST_STRPTR)"timer.device");
    Permit();
    clock_mode = 2;
}

void nc_now_ticks(ULONG *hi, ULONG *lo, ULONG *freq)
{
    if (!clock_mode)
        clock_setup();
    if (clock_mode == 1)
    {
        do
            Supervisor((ULONG (*)())clock_code);
        while (clock_out[0] != clock_out[2]);       /* low wrapped between the reads */
        *hi = clock_out[0];
        *lo = clock_out[1];
        *freq = clock_out[3];
    }
    else
    {
        struct EClockVal ev;
        struct Device *TimerBase = clock_timer;

        *freq = TimerBase ? ReadEClock(&ev) : 0;
        *hi = TimerBase ? ev.ev_hi : 0;
        *lo = TimerBase ? ev.ev_lo : 0;
    }
}

ULONG nc_now_us(void)
{
    ULONG hi, lo, freq;
    unsigned long long ticks;

    nc_now_ticks(&hi, &lo, &freq);
    if (!freq)
        return 0;
    ticks = ((unsigned long long)hi << 32) | lo;
    if (freq % 1000000UL == 0)
        return (ULONG)(ticks / (freq / 1000000UL));         /* Emu68: 54 MHz */
    return (ULONG)(ticks / freq * 1000000ULL + ticks % freq * 1000000ULL / freq);
}

APTR nc_alloc(ULONG size)
{
    if (NativeCallBase)
        return NC_AllocMem(size);
    return AllocMem(size, MEMF_ANY | MEMF_CLEAR);
}

void nc_free(APTR memory, ULONG size)
{
    if (!memory)
        return;
    if (NativeCallBase)
        NC_FreeMem(memory, size);
    else
        FreeMem(memory, size);
}
