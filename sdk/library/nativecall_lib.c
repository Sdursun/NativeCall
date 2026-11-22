/*
    nativecall.library - 68k side of nativecall (NOTES.md 9, sdk/README.md).

    A request: fill the 64-byte call block, nc_trigger() runs CPUSHL DC,(A0) on it in
    supervisor mode with the doorbell in D0/D1. Emu68 turns that into "dc civac", the
    hypervisor traps it and answers in the block. Without the hypervisor the block's status
    stays NC_ST_NONE.

    One call block for all callers: a request runs under Forbid() and is short (the caller
    keeps native calls around 1 ms), so tasks simply take turns. The module list is protected
    by a semaphore. Built with VBCC, registerised entry points, no startup code.

    1.8: without the hypervisor, under WinUAE with uaenative.library (native_code=true) and
    nativecall-windows-x86-64.dll in WinUAE's plugins folder, kernels run on the PC's x64 CPU
    (x64/README.md). A program loads its .narm as before; the library looks for the .nx64 of the
    same build (CRC32 of the .narm) in ENV:NativeCall/ModuleDir and PROGDIR: and hands it to the
    DLL. Programs do not change. Asynchronous jobs run on the DLL's thread pool while the
    emulation goes on. Not there yet: time limit, log, fault details.
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
#include <dos/dosextens.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/timer.h>

#include "nc_abi.h"

#define LIB_VERSION     1
#define LIB_REVISION    8

/* Native code cannot see the low 16 MB (chip RAM, Z2 space, I/O): hypervisor/hv.c */
#define NC_LOW_LIMIT    0x01000000UL

/* NC_Run: aim for this much native time per call (us) */
#define RUN_TARGET_US   500

/* Zeroed bytes kept behind a module image. Modules built with the SDK between 02.11.2026 and
   12.11.2026 lack their .bss (static variables of kernels that start as zero): the image ended
   before it, and kernels wrote past the allocation into the free list of exec (NOTES.md 26.7).
   The SDK now puts .bss into the image; this margin makes such older modules safe as long as
   their .bss is not larger. */
#define MODULE_SLACK    0x10000

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
    ULONG                   features;       /* NC_FEAT_* of the first PING (hypervisor 6+) */
    /* Bounce buffer for NC_ReadLog / NC_GetFaultInfo: the hypervisor only writes Fast RAM above
       16 MB, the caller's buffer may be anywhere */
    UBYTE                  *bounce;
    struct SignalSemaphore  bounce_sem;
    /* NC_StartMP jobs (hypervisor 4): slot n = hypervisor job n, handle n + 1 */
    volatile struct NCJobStatus *job_status;    /* NC_ASYNC_JOBS, Fast RAM, for the hypervisor */
    APTR                    job_status_alloc;
    struct NCModule        *job_module[NC_ASYNC_JOBS];   /* NULL: slot free */
    UBYTE                   job_block[NC_ASYNC_JOBS];    /* its entry in job_status[] */
    struct Task            *job_task[NC_ASYNC_JOBS];     /* who started it (reap_jobs) */
    /* 1.8: x64 backend under WinUAE (uaenative.library + nativecall-windows-x86-64.dll) */
    struct SignalSemaphore  init_sem;       /* the first available() */
    BOOL                    x64;
    struct Library         *uni_base;
    ULONG                   uni_host;
    ULONG                   f_call, f_run, f_load, f_unload, f_shutdown;
    ULONG                   f_start, f_poll, f_wait, f_cancel;  /* asynchronous jobs (optional) */
    ULONG                   f_logread, f_faultinfo, f_stats;    /* host 2 (optional) */
    ULONG                   job_x64[NC_ASYNC_JOBS];             /* x64: the host's job of a slot */
};

struct NCModule
{
    struct MinNode  node;
    APTR            alloc;
    ULONG           alloc_size;
    UBYTE          *base;
    ULONG           size;
    ULONG           timeout_us;     /* time limit of one native call, 0 = hypervisor default */
    ULONG           calls;          /* requests made for it (NC_ListModules) */
    ULONG           total_us;       /* their native time, as far as measured */
    ULONG           faults;         /* requests that ended in a fault or time-out */
    char            owner[32];      /* program that loaded it */
    ULONG           x64_handle;     /* x64 backend: the host DLL's handle; base is then a made-up
                                       NativeHeader whose entries are 16, 32, ... */
};

struct ExecBase *SysBase;
struct DosLibrary *DOSBase;
struct Device *TimerBase;

void nc_trigger(__reg("a0") volatile struct NCBlock *block);
ULONG LIB_NC_WaitJob(__reg("a6") struct NCBase *nb, __reg("d0") ULONG job, __reg("a0") ULONG *status);
ULONG LIB_NC_CancelJob(__reg("a6") struct NCBase *nb, __reg("d0") ULONG job, __reg("a0") ULONG *status);

const char LibName[] = "nativecall.library";
const char LibIdString[] = "nativecall.library 1.8 (21.11.2026)\r\n";
const char LibVersTag[] = "$VER: nativecall.library 1.8 (21.11.2026)";

/* ---- uaenative.library (WinUAE, uaenative.cpp): handles have bit 31 set ---- */

ULONG nc_uni_call(__reg("a6") struct Library *uni, __reg("d0") ULONG function, __reg("a0") ULONG *regs);
ULONG uni_open_library(__reg("a6") struct Library *uni, __reg("a1") CONST_STRPTR name, __reg("d0") ULONG min_version) = "\tjsr\t-30(a6)";
VOID uni_close_library(__reg("a6") struct Library *uni, __reg("a1") ULONG library) = "\tjsr\t-36(a6)";
ULONG uni_get_function(__reg("a6") struct Library *uni, __reg("a0") ULONG library, __reg("a1") CONST_STRPTR name) = "\tjsr\t-42(a6)";
#define UNI_VALID(h)    (((h) & 0x80000000UL) != 0)

/* register slots of nc_uni_call */
enum { R_D1, R_D2, R_D3, R_D4, R_D5, R_D6, R_D7, R_A1, R_A2, R_A3, R_A4, R_A5 };

/* the oldest nativecall-windows-x86-64.dll this library works with; 2 adds faults, time limit, log,
   statistics (each looked up on its own) */
#define NCX_HOST_VERSION    1

/* ---- Memory ---- */

static BOOL x64_mode;   /* memory for kernels may be anywhere: the host DLL checks it */

static APTR alloc_high(ULONG size)
{
    APTR p;
    struct MemHeader *mh;

    if (x64_mode)
    {
        p = AllocMem(size, MEMF_FAST | MEMF_PUBLIC | MEMF_CLEAR);
        return p ? p : AllocMem(size, MEMF_ANY | MEMF_PUBLIC | MEMF_CLEAR);
    }

    p = AllocMem(size, MEMF_FAST | MEMF_PUBLIC | MEMF_CLEAR);

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

/* in: NULL or 5 ULONGs for info[0..4] (NC_OP_RUNMP, NC_OP_START); info: NULL or 8 ULONGs out;
   limit: time limit of the native code in us (info[5], hypervisor 6+), 0 = its default */
static ULONG request(struct NCBase *nb, ULONG op, ULONG a0, ULONG a1, ULONG a2, ULONG *status,
                     const ULONG *in, ULONG *info, ULONG limit)
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
    b->info[NC_CALLINFO_TIMEOUT] = limit;   /* always: a fault leaves its ELR here */
    b->info[NC_CALLINFO_TICKS] = 0;     /* hypervisor 1 does not set them */
    b->info[NC_CALLINFO_FREQ] = 0;
    b->magic = NC_BLOCK_MAGIC;

    nc_trigger(b);

    st = b->status;
    result = b->result;
    if (st != NC_ST_NONE)
        nb->hv_version = b->version;
    if (st == NC_ST_FAULT || st == NC_ST_TIMEOUT)
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

/* x64 backend: uaenative.library and the host DLL; not under Forbid() (OpenLibrary may load from
   disk). TRUE if both are there and fit. */
static ULONG x64_function(struct NCBase *nb, CONST_STRPTR name)
{
    ULONG f = uni_get_function(nb->uni_base, nb->uni_host, name);
    return UNI_VALID(f) ? f : 0;
}

static BOOL x64_probe(struct NCBase *nb)
{
    ULONG regs[12] = {0}, f_version, f_cores, f_features;

    if (!(nb->uni_base = OpenLibrary((CONST_STRPTR)"uaenative.library", 1)))
        return FALSE;
    nb->uni_host = uni_open_library(nb->uni_base, (CONST_STRPTR)"nativecall", 0);
    if (UNI_VALID(nb->uni_host))
    {
        f_version = x64_function(nb, (CONST_STRPTR)"ncx_version");
        f_cores = x64_function(nb, (CONST_STRPTR)"ncx_cores");
        nb->f_call = x64_function(nb, (CONST_STRPTR)"ncx_call");
        nb->f_run = x64_function(nb, (CONST_STRPTR)"ncx_run");
        nb->f_load = x64_function(nb, (CONST_STRPTR)"ncx_load");
        nb->f_unload = x64_function(nb, (CONST_STRPTR)"ncx_unload");
        nb->f_shutdown = x64_function(nb, (CONST_STRPTR)"ncx_shutdown");     /* optional */
        nb->f_start = x64_function(nb, (CONST_STRPTR)"ncx_start");
        nb->f_poll = x64_function(nb, (CONST_STRPTR)"ncx_poll");
        nb->f_wait = x64_function(nb, (CONST_STRPTR)"ncx_wait");
        nb->f_cancel = x64_function(nb, (CONST_STRPTR)"ncx_cancel");
        nb->f_logread = x64_function(nb, (CONST_STRPTR)"ncx_logread");
        nb->f_faultinfo = x64_function(nb, (CONST_STRPTR)"ncx_faultinfo");
        nb->f_stats = x64_function(nb, (CONST_STRPTR)"ncx_stats");
        f_features = x64_function(nb, (CONST_STRPTR)"ncx_features");
        if (f_version && f_cores && nb->f_call && nb->f_run && nb->f_load && nb->f_unload
            && nc_uni_call(nb->uni_base, f_version, regs) >= NCX_HOST_VERSION)
        {
            nb->cores = nc_uni_call(nb->uni_base, f_cores, regs);
            if (nb->cores < 1)
                nb->cores = 1;
            nb->x64 = TRUE;
            nb->features = f_features ? nc_uni_call(nb->uni_base, f_features, regs) : NC_FEAT_X64;
            x64_mode = TRUE;
            return TRUE;
        }
        uni_close_library(nb->uni_base, nb->uni_host);
    }
    nb->uni_host = 0;
    CloseLibrary(nb->uni_base);
    nb->uni_base = NULL;
    return FALSE;
}

static BOOL available(struct NCBase *nb)
{
    ULONG st;

    if (nb->available >= 0)
        return (BOOL)nb->available;

    ObtainSemaphore(&nb->init_sem);
    Forbid();
    if (nb->available < 0)
    {
        /* CPUSHL exists on 68040/68060 only (Emu68 reports a 68040) */
        if (SysBase->AttnFlags & AFF_68040)
        {
            nb->block_alloc = alloc_high(2 * NC_BLOCK_SIZE);
            if (nb->block_alloc)
            {
                nb->block = (volatile struct NCBlock *)(((ULONG)nb->block_alloc + NC_BLOCK_SIZE - 1) & ~(ULONG)(NC_BLOCK_SIZE - 1));
                ULONG info[8], helpers;

                if (request(nb, NC_OP_PING, 0, 0, 0, &st, NULL, info, 0) == NC_PING_REPLY && st == NC_ST_OK)
                {
                    nb->available = 1;
                    nb->cores = 1;
                    if (nb->hv_version >= 3)
                        for (helpers = info[NC_PINGINFO_HELPERS] & 0xe; helpers; helpers >>= 1)
                            nb->cores += helpers & 1;
                    if (nb->hv_version >= 6)
                        nb->features = info[NC_PINGINFO_FEATURES];
                }
            }
        }
    }
    Permit();
    /* no hypervisor: WinUAE's x64 backend */
    if (nb->available < 0)
        nb->available = x64_probe(nb) ? 1 : 0;
    ReleaseSemaphore(&nb->init_sem);

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

static void free_module(struct NCBase *nb, struct NCModule *m)
{
    if (m->x64_handle)
    {
        ULONG regs[12] = {0};

        regs[R_D1] = m->x64_handle;
        nc_uni_call(nb->uni_base, nb->f_unload, regs);
    }
    FreeMem(m->alloc, m->alloc_size);
    FreeMem(m, sizeof(*m));
}

/* Name of the program that calls: the CLI command without its path, or the task's name */
static void owner_name(char *dst, ULONG size)
{
    struct Task *t = SysBase->ThisTask;
    const UBYTE *s = (const UBYTE *)t->tc_Node.ln_Name;
    ULONG len = 0, i, start = 0;

    if (s)
        while (s[len])
            len++;
    if (t->tc_Node.ln_Type == NT_PROCESS)
    {
        struct CommandLineInterface *cli = BADDR(((struct Process *)t)->pr_CLI);
        const UBYTE *b = cli ? (const UBYTE *)BADDR(cli->cli_CommandName) : NULL;

        if (b && b[0])
        {
            s = b + 1;
            len = b[0];
        }
    }
    for (i = 0; i < len; i++)
        if (s[i] == '/' || s[i] == ':')
            start = i + 1;
    for (i = 0; start + i < len && i < size - 1; i++)
        dst[i] = (char)s[start + i];
    dst[i] = 0;
}

/* Copy a module image into Fast RAM above 16 MB, 4 KB aligned (the SDK builds with
   -mcmodel=tiny, so any 16-byte address works, but code using ADRP only works at its link
   address modulo 4 KB: NOTES.md 15), check it and make the ARM I-cache coherent.
   fill() writes the image to the destination, with ctx. */
static struct NCModule *load_module_x64(struct NCBase *nb, ULONG size, BOOL (*fill)(UBYTE *dst, ULONG size, APTR ctx), APTR ctx);

static struct NCModule *load_module(struct NCBase *nb, ULONG size, BOOL (*fill)(UBYTE *dst, ULONG size, APTR ctx), APTR ctx)
{
    struct NCModule *m;
    ULONG st;

    if (!available(nb))
        return NULL;
    if (nb->x64)
        return load_module_x64(nb, size, fill, ctx);
    if (size < 16 || size > 0x100000)
        return NULL;

    m = AllocMem(sizeof(*m), MEMF_PUBLIC | MEMF_CLEAR);
    if (!m)
        return NULL;
    m->alloc_size = size + 4095 + MODULE_SLACK;
    m->alloc = alloc_high(m->alloc_size);
    if (!m->alloc)
    {
        FreeMem(m, sizeof(*m));
        return NULL;
    }
    m->base = (UBYTE *)(((ULONG)m->alloc + 4095) & ~4095UL);
    m->size = size;
    owner_name(m->owner, sizeof(m->owner));

    if (!fill(m->base, size, ctx) || !check_header((struct NativeHeader *)m->base, size))
    {
        free_module(nb, m);
        return NULL;
    }

    CacheClearU();

    /* The ARM I-cache does not see 68k cache operations (NOTES.md 6) */
    request(nb, NC_OP_SYNC, (ULONG)m->base, m->size, 0, &st, NULL, NULL, 0);
    if (st != NC_ST_OK)
    {
        free_module(nb, m);
        return NULL;
    }

    ObtainSemaphore(&nb->sem);
    AddTail((struct List *)&nb->modules, (struct Node *)&m->node);
    ReleaseSemaphore(&nb->sem);
    return m;
}

/* ---- x64 modules (WinUAE) ---- */

/* CRC32 as zlib: the build ID of a .narm (tools/narm2c.py, tools/mknx64.py) */
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

static BOOL nx64_header_ok(const struct NX64Header *h, ULONG size)
{
    return (BOOL)(size >= sizeof(*h) && h->magic == NX64_MAGIC && h->version == NX64_VERSION
                  && h->dll_offset == sizeof(*h) && h->dll_size <= size - sizeof(*h)
                  && h->entry_count > 0 && h->entry_count <= NARM_MAX_ENTRIES);
}

/* Reads a whole file into memory; *size its length. NULL if it cannot. */
static UBYTE *read_file(CONST_STRPTR path, ULONG *size)
{
    BPTR fh = Open(path, MODE_OLDFILE);
    UBYTE *buf = NULL;
    LONG n;

    if (!fh)
        return NULL;
    Seek(fh, 0, OFFSET_END);
    n = Seek(fh, 0, OFFSET_BEGINNING);
    if (n > 0 && n <= 0x1000000 && (buf = AllocMem(n, MEMF_ANY)))
    {
        if (Read(fh, buf, n) == n)
            *size = (ULONG)n;
        else
        {
            FreeMem(buf, n);
            buf = NULL;
        }
    }
    Close(fh);
    return buf;
}

/* The .nx64 of the same build as a .narm (its CRC32 and kernel count) in one directory */
static UBYTE *find_nx64_in(CONST_STRPTR dir, ULONG id, ULONG count, ULONG *size)
{
    struct FileInfoBlock *fib;
    struct NX64Header h;
    char path[256];
    UBYTE *image = NULL;
    BPTR lock, fh;
    ULONG len;

    if (!(lock = Lock(dir, SHARED_LOCK)))
        return NULL;
    fib = AllocDosObject(DOS_FIB, NULL);
    if (fib && Examine(lock, fib))
    {
        while (!image && ExNext(lock, fib))
        {
            const char *name = (const char *)fib->fib_FileName;

            for (len = 0; name[len]; len++)
                ;
            if (fib->fib_DirEntryType >= 0 || len < 6 || name[len - 5] != '.'
                || (name[len - 4] | 0x20) != 'n' || (name[len - 3] | 0x20) != 'x'
                || name[len - 2] != '6' || name[len - 1] != '4' || fib->fib_Size < (LONG)sizeof(h))
                continue;
            for (len = 0; dir[len] && len < sizeof(path) - 1; len++)
                path[len] = (char)dir[len];
            path[len] = 0;
            if (!AddPart((STRPTR)path, (CONST_STRPTR)name, sizeof(path)))
                continue;
            if (!(fh = Open((CONST_STRPTR)path, MODE_OLDFILE)))
                continue;
            len = Read(fh, &h, sizeof(h));
            Close(fh);
            if (len == sizeof(h) && nx64_header_ok(&h, (ULONG)fib->fib_Size) && h.narm_id == id
                && h.entry_count == count)
                image = read_file((CONST_STRPTR)path, size);
        }
    }
    if (fib)
        FreeDosObject(DOS_FIB, fib);
    UnLock(lock);
    return image;
}

/* Where nc_open looks for the .narm: ENV:NativeCall/ModuleDir, then PROGDIR: */
static UBYTE *find_nx64(ULONG id, ULONG count, ULONG *size)
{
    struct Task *t = SysBase->ThisTask;
    char dir[200];
    UBYTE *image = NULL;

    /* DOS needs a process */
    if (t->tc_Node.ln_Type != NT_PROCESS)
        return NULL;
    if (GetVar((CONST_STRPTR)"NativeCall/ModuleDir", (STRPTR)dir, sizeof(dir), 0) > 0)
        image = find_nx64_in((CONST_STRPTR)dir, id, count, size);
    if (!image)
        image = find_nx64_in((CONST_STRPTR)"PROGDIR:", id, count, size);
    return image;
}

/* A .narm (its .nx64 is looked up) or a .nx64 image: loaded into the host DLL. The module's
   base is a made-up NativeHeader with entries 16, 32, ...: entry pointers stay unique and the
   rest of the library (find_module, NC_GetEntry, statistics) works unchanged. */
static struct NCModule *load_module_x64(struct NCBase *nb, ULONG size, BOOL (*fill)(UBYTE *dst, ULONG size, APTR ctx), APTR ctx)
{
    struct NCModule *m;
    struct NativeHeader *h;
    UBYTE *buf, *image = NULL;
    ULONG image_size = 0, count = 0, regs[12] = {0}, i;

    if (size < 16 || size > 0x1000000 || !(buf = AllocMem(size, MEMF_ANY)))
        return NULL;
    if (fill(buf, size, ctx))
    {
        if (nx64_header_ok((struct NX64Header *)buf, size))
        {
            image = buf;
            image_size = size;
            count = ((struct NX64Header *)buf)->entry_count;
            buf = NULL;
        }
        else if (check_header((struct NativeHeader *)buf, size))
        {
            count = ((struct NativeHeader *)buf)->entry_count;
            image = find_nx64(crc32(buf, size), count, &image_size);
        }
    }
    if (buf)
        FreeMem(buf, size);
    if (!image)
        return NULL;

    regs[R_D1] = image_size;
    regs[R_A1] = (ULONG)image;
    regs[R_D2] = nc_uni_call(nb->uni_base, nb->f_load, regs);
    FreeMem(image, image_size);
    if (!regs[R_D2])
        return NULL;

    m = AllocMem(sizeof(*m), MEMF_PUBLIC | MEMF_CLEAR);
    if (m)
    {
        m->x64_handle = regs[R_D2];
        m->size = m->alloc_size = 16 * (count + 1);
        m->alloc = AllocMem(m->alloc_size, MEMF_PUBLIC | MEMF_CLEAR);
    }
    if (!m || !m->alloc)
    {
        regs[R_D1] = regs[R_D2];
        nc_uni_call(nb->uni_base, nb->f_unload, regs);
        if (m)
            FreeMem(m, sizeof(*m));
        return NULL;
    }
    m->base = m->alloc;
    h = (struct NativeHeader *)m->base;
    h->magic = NARM_MAGIC;
    h->version = NARM_VERSION;
    h->entry_count = count;
    for (i = 0; i < count; i++)
        h->entries[i] = 16 * (i + 1);
    owner_name(m->owner, sizeof(m->owner));

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

    /* jobs still running in this module's code: stop them first (1.6: cancelled, not waited
       for; the pieces that run right now still end) */
    for (i = 0; i < NC_ASYNC_JOBS; i++)
        if (nb->job_module[i] == m)
            LIB_NC_CancelJob(nb, i + 1, NULL);

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

    free_module(nb, m);
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

/* x64: NC_LastFault from the host DLL's fault details (Windows exception code, address, instruction) */
static UBYTE *bounce(struct NCBase *nb);

static void x64_last_fault(struct NCBase *nb)
{
    struct NCFaultInfo *fi;
    ULONG regs[12] = {0};

    if (!nb->f_faultinfo || !(fi = (struct NCFaultInfo *)bounce(nb)))
        return;
    ObtainSemaphore(&nb->bounce_sem);
    regs[R_A1] = (ULONG)fi;
    regs[R_D1] = sizeof(*fi);
    if (nc_uni_call(nb->uni_base, nb->f_faultinfo, regs))
    {
        nb->last_fault[0] = fi->esr;
        nb->last_fault[1] = fi->far_lo;
        nb->last_fault[2] = fi->elr_lo;
    }
    ReleaseSemaphore(&nb->bounce_sem);
}

/* One native call; *us = native run time (0xffffffff: the hypervisor does not measure).
   mp: NULL for NC_OP_CALL, or the 4 input words of NC_OP_RUNMP. */
static ULONG call(struct NCBase *nb, APTR entry, APTR params, struct NCModule *m, ULONG *status, ULONG *us,
                  const ULONG *mp)
{
    ULONG info[8], result, per_us;
    struct KernelInfo *ki;

    if (m->x64_handle)
    {
        /* host DLL: out = status, microseconds, threads (sdk/x64/ncx_host.c) */
        ULONG regs[12] = {0}, out[4];

        out[0] = NC_ST_BADENTRY;
        out[1] = 0;
        regs[R_D1] = m->x64_handle;
        regs[R_D2] = ((ULONG)((UBYTE *)entry - m->base) >> 4) - 1;
        regs[R_A1] = (ULONG)params;
        regs[R_A2] = (ULONG)out;
        if (mp)
        {
            regs[R_D3] = mp[NC_MPINFO_FROM];
            regs[R_D4] = mp[NC_MPINFO_TO];
            regs[R_D5] = mp[NC_MPINFO_CHUNK];
            regs[R_D6] = mp[NC_MPINFO_SIZE];
            regs[R_A3] = m->timeout_us;         /* host 2: time limit of a piece */
            result = nc_uni_call(nb->uni_base, nb->f_run, regs);
        }
        else
        {
            regs[R_D3] = m->timeout_us;
            result = nc_uni_call(nb->uni_base, nb->f_call, regs);
        }
        *status = out[0];
        *us = out[1];
        m->calls++;
        if (*status == NC_ST_FAULT || *status == NC_ST_TIMEOUT)
        {
            m->faults++;
            x64_last_fault(nb);
        }
    }
    else
    {
        result = request(nb, mp ? NC_OP_RUNMP : NC_OP_CALL, (ULONG)entry, (ULONG)params, (ULONG)m->base,
                         status, mp, info, m->timeout_us);
        m->calls++;
        if (*status == NC_ST_FAULT || *status == NC_ST_TIMEOUT)
            m->faults++;
        per_us = info[NC_CALLINFO_FREQ] / 1000000;      /* 54 on a Pi 4: no 64-bit division */
        if (per_us == 0)
        {
            *us = 0xffffffff;
            return result;
        }
        *us = info[NC_CALLINFO_TICKS] / per_us;
    }
    m->total_us += *us;

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
    if (!available(nb) || nb->cores < 2 || (!nb->x64 && nb->hv_version < 3) || params_size < 8
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
    if (available(nb) && nb->x64)
        return (BOOL)(nb->f_start && nb->f_poll && nb->f_wait && nb->f_cancel);
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
/* Is this task still there? (Forbid() held by the caller) */
static BOOL task_alive(struct Task *t)
{
    struct Node *n;

    if (t == SysBase->ThisTask)
        return TRUE;
    for (n = SysBase->TaskReady.lh_Head; n->ln_Succ; n = n->ln_Succ)
        if ((struct Task *)n == t)
            return TRUE;
    for (n = SysBase->TaskWait.lh_Head; n->ln_Succ; n = n->ln_Succ)
        if ((struct Task *)n == t)
            return TRUE;
    return FALSE;
}

/* Jobs of tasks that are gone (a program that ended or crashed without NC_WaitJob): cancel
   them, so that the helper cores stop writing memory nobody owns any more and the job slots
   come back. Called when a job starts and when the library is opened: there is no hook for
   the end of a task, so until then such a job runs on. */
static void reap_jobs(struct NCBase *nb)
{
    ULONG dead = 0;
    int i;

    Forbid();
    for (i = 0; i < NC_ASYNC_JOBS; i++)
        if (nb->job_module[i] && !task_alive(nb->job_task[i]))
            dead |= 1UL << i;
    Permit();
    for (i = 0; i < NC_ASYNC_JOBS; i++)
        if (dead & (1UL << i))
            LIB_NC_CancelJob(nb, i + 1, NULL);
}

ULONG LIB_NC_StartMP(__reg("a6") struct NCBase *nb, __reg("a0") APTR entry, __reg("a1") APTR params,
                     __reg("d0") ULONG from, __reg("d1") ULONG to, __reg("d2") ULONG params_size,
                     __reg("d3") ULONG chunk)
{
    struct NCModule *m;
    struct KernelInfo *ki;
    ULONG in[5], st, slot;

    if (!async_ready(nb) || from > to || !(m = find_module(nb, entry)))
        return 0;
    reap_jobs(nb);
    if (!chunk)
    {
        Forbid();
        ki = kernel_info(nb, entry, FALSE);
        chunk = ki && ki->learned[0] ? ki->step[0] : (to - from) / 16;
        Permit();
        if (!chunk)
            chunk = 1;
        /* x64: many more threads than ARM cores, so smaller pieces for an even share */
        if (nb->x64 && chunk > (to - from) / (4 * nb->cores))
            chunk = (to - from) / (4 * nb->cores) ? (to - from) / (4 * nb->cores) : 1;
    }
    if (nb->x64)
    {
        ULONG regs[12] = {0}, id;

        Forbid();
        for (slot = 0; slot < NC_ASYNC_JOBS && nb->job_module[slot]; slot++)
            ;
        if (slot < NC_ASYNC_JOBS)
        {
            regs[R_D1] = m->x64_handle;
            regs[R_D2] = ((ULONG)((UBYTE *)entry - m->base) >> 4) - 1;
            regs[R_D3] = from;
            regs[R_D4] = to;
            regs[R_D5] = chunk;
            regs[R_D6] = params_size;
            regs[R_A1] = (ULONG)params;
            regs[R_A2] = m->timeout_us;         /* host 2 */
            id = nc_uni_call(nb->uni_base, nb->f_start, regs);
            if (id)
            {
                nb->job_module[slot] = m;
                nb->job_task[slot] = SysBase->ThisTask;
                nb->job_x64[slot] = id;
                m->calls++;
            }
            else
                slot = NC_ASYNC_JOBS;
        }
        Permit();
        return slot < NC_ASYNC_JOBS ? slot + 1 : 0;
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
    slot = request(nb, NC_OP_START, (ULONG)entry, (ULONG)params, (ULONG)m->base, &st, in, NULL, m->timeout_us);
    if (st == NC_ST_OK && slot < NC_ASYNC_JOBS)
    {
        /* the hypervisor's slot and ours agree (both free a slot on WAIT); keep the link anyway */
        nb->job_module[slot] = m;
        m->calls++;
        nb->job_task[slot] = SysBase->ThisTask;
        nb->job_block[slot] = (UBYTE)(((ULONG)in[NC_JOBINFO_STATUS] - (ULONG)nb->job_status) / sizeof(struct NCJobStatus));
    }
    Permit();

    return st == NC_ST_OK && slot < NC_ASYNC_JOBS ? slot + 1 : 0;
}

/* TRUE when the job is complete (reads memory only, no request) */
BOOL LIB_NC_JobDone(__reg("a6") struct NCBase *nb, __reg("d0") ULONG job)
{
    if (job >= 1 && job <= NC_ASYNC_JOBS && nb->x64 && nb->job_module[job - 1])
    {
        ULONG regs[12] = {0};

        regs[R_D1] = nb->job_x64[job - 1];
        return (BOOL)(nc_uni_call(nb->uni_base, nb->f_poll, regs) != 0);
    }
    if (job < 1 || job > NC_ASYNC_JOBS || !nb->job_status || !nb->job_module[job - 1])
        return TRUE;
    return (BOOL)(nb->job_status[nb->job_block[job - 1]].done != 0);
}

/* Waits for the job (core 0 helps in steps of at most 0.5 ms, the 68k runs in between) and
   returns the sum of its results. *status (may be NULL): NC_ST_OK, NC_ST_FAULT, NC_ST_TIMEOUT,
   NC_ST_CANCELLED, NC_ST_BADJOB.
   The job's output may be read after this, not before. */
ULONG LIB_NC_WaitJob(__reg("a6") struct NCBase *nb, __reg("d0") ULONG job, __reg("a0") ULONG *status)
{
    ULONG st = NC_ST_BADJOB, result = 0xffffffff;

    if (job >= 1 && job <= NC_ASYNC_JOBS && nb->job_module[job - 1] && nb->x64)
    {
        /* the emulation thread helps for at most 0.5 ms, then the 68k runs a little */
        ULONG regs[12] = {0}, out[4];

        do
        {
            out[0] = NC_ST_BADJOB;
            regs[R_D1] = nb->job_x64[job - 1];
            regs[R_D2] = 500;
            regs[R_A2] = (ULONG)out;
            result = nc_uni_call(nb->uni_base, nb->f_wait, regs);
            st = out[0];
        }
        while (st == NC_ST_RUNNING);
        if (st == NC_ST_OK || st == NC_ST_CANCELLED)
            nb->job_module[job - 1]->total_us += out[1];
        else if (st == NC_ST_FAULT || st == NC_ST_TIMEOUT)
        {
            nb->job_module[job - 1]->faults++;
            x64_last_fault(nb);
        }
        nb->job_module[job - 1] = NULL;
    }
    else if (job >= 1 && job <= NC_ASYNC_JOBS && nb->job_module[job - 1])
    {
        do
            result = request(nb, NC_OP_WAIT, job - 1, 0, 0, &st, NULL, NULL, 0);
        while (st == NC_ST_RUNNING);
        nb->job_module[job - 1] = NULL;
    }
    if (status)
        *status = st;
    return result;
}

/* 1.6: stops a job: pieces that have not started are skipped, pieces that run right now end (at
   most the time limit), then as NC_WaitJob. *status: NC_ST_CANCELLED and the sum of the pieces
   that ran, or the job's own result if it was complete already. The job's output is incomplete.
   With a hypervisor that cannot cancel this is NC_WaitJob. */
ULONG LIB_NC_CancelJob(__reg("a6") struct NCBase *nb, __reg("d0") ULONG job, __reg("a0") ULONG *status)
{
    ULONG st;

    if (job >= 1 && job <= NC_ASYNC_JOBS && nb->job_module[job - 1] && nb->x64)
    {
        ULONG regs[12] = {0};

        regs[R_D1] = nb->job_x64[job - 1];
        nc_uni_call(nb->uni_base, nb->f_cancel, regs);
    }
    else if (job >= 1 && job <= NC_ASYNC_JOBS && nb->job_module[job - 1])
        request(nb, NC_OP_CANCEL, job - 1, 0, 0, &st, NULL, NULL, 0);
    return LIB_NC_WaitJob(nb, job, status);
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

    if (!available(nb) || nb->x64)
        return FALSE;
    request(nb, NC_OP_INFO, 0, 0, 0, &st, NULL, info, 0);
    return (BOOL)(st == NC_ST_OK);
}

VOID LIB_NC_LastFault(__reg("a6") struct NCBase *nb, __reg("a0") ULONG *fault)
{
    fault[0] = nb->last_fault[0];
    fault[1] = nb->last_fault[1];
    fault[2] = nb->last_fault[2];
}

/* 1.6: time limit of one native call of this module's kernels (one piece for NC_Run, NC_RunMP,
   NC_StartMP), in microseconds; 0 = the hypervisor's default (NC_TIMEOUT_DEFAULT_US, 1 s).
   Past it the call ends with NC_ST_TIMEOUT instead of hanging the machine (hypervisor 6+;
   older ones have no limit). Per module, so programs do not change each other's limit.
   Returns the previous value, or 0xFFFFFFFF if there is no working time limit on this machine
   (the value is still kept). */
ULONG LIB_NC_SetTimeout(__reg("a6") struct NCBase *nb, __reg("a0") APTR module, __reg("d0") ULONG us)
{
    struct NCModule *m = module;
    ULONG old;

    if (!m)
        return 0xffffffff;
    old = (nb->features & NC_FEAT_TIMEOUT) ? m->timeout_us : 0xffffffff;
    m->timeout_us = us;
    return old;
}

/* ---- Log and fault details (hypervisor 7, nc_abi.h) ---- */

#define BOUNCE_SIZE     1024

static UBYTE *bounce(struct NCBase *nb)
{
    if (!nb->bounce)
    {
        Forbid();
        if (!nb->bounce)
            nb->bounce = alloc_high(BOUNCE_SIZE);
        Permit();
    }
    return nb->bounce;
}

/* 1.6: copies up to size bytes of the hypervisor's log (text of nc_printf and the hypervisor's
   own lines about faults and time-outs) from *position on, and moves *position past them. Start
   with *position = 0. Returns the bytes copied, 0 when there is nothing new or no log
   (hypervisor < 7). Text that was overwritten before it could be read is skipped. */
ULONG LIB_NC_ReadLog(__reg("a6") struct NCBase *nb, __reg("a0") APTR buffer, __reg("d0") ULONG size,
                     __reg("a1") ULONG *position)
{
    ULONG info[8], st, n = 0, part;
    UBYTE *b;

    if (buffer && position && available(nb) && nb->x64)
    {
        /* the host DLL writes into the buffer itself */
        ULONG regs[12] = {0}, out[2];

        if (!nb->f_logread)
            return 0;
        regs[R_A1] = (ULONG)buffer;
        regs[R_D1] = size;
        regs[R_D2] = *position;
        regs[R_A2] = (ULONG)out;
        out[0] = *position;
        n = nc_uni_call(nb->uni_base, nb->f_logread, regs);
        *position = out[0];
        return n;
    }

    if (!buffer || !position || !available(nb) || nb->hv_version < 7 || !(b = bounce(nb)))
        return 0;
    ObtainSemaphore(&nb->bounce_sem);
    while (n < size)
    {
        part = size - n < BOUNCE_SIZE ? size - n : BOUNCE_SIZE;
        part = request(nb, NC_OP_LOGREAD, (ULONG)b, part, *position, &st, NULL, info, 0);
        if (st != NC_ST_OK || part == 0)
            break;
        CopyMem(b, (UBYTE *)buffer + n, part);
        *position = info[NC_LOGINFO_POS];
        n += part;
    }
    ReleaseSemaphore(&nb->bounce_sem);
    return n;
}

/* 1.6: details of the last native fault or time-out on any core: registers and a backtrace as
   module offsets (nc addr2line on the PC). FALSE: none yet, or hypervisor < 7. */
BOOL LIB_NC_GetFaultInfo(__reg("a6") struct NCBase *nb, __reg("a0") struct NCFaultInfo *fi)
{
    ULONG st;
    UBYTE *b;
    BOOL ok;

    if (fi && available(nb) && nb->x64)
    {
        ULONG regs[12] = {0};

        if (!nb->f_faultinfo)
            return FALSE;
        regs[R_A1] = (ULONG)fi;
        regs[R_D1] = sizeof(*fi);
        return (BOOL)(nc_uni_call(nb->uni_base, nb->f_faultinfo, regs) != 0);
    }

    if (!fi || !available(nb) || nb->hv_version < 7 || !(b = bounce(nb)))
        return FALSE;
    ObtainSemaphore(&nb->bounce_sem);
    request(nb, NC_OP_FAULTINFO, (ULONG)b, sizeof(*fi), 0, &st, NULL, NULL, 0);
    ok = st == NC_ST_OK && ((struct NCFaultInfo *)b)->status != 0;
    if (ok)
        CopyMem(b, fi, sizeof(*fi));
    ReleaseSemaphore(&nb->bounce_sem);
    return ok;
}

/* 1.7: statistics of the hypervisor (8+) for a monitor: native time of each ARM core, SoC
   temperature, jobs, faults. FALSE without them. */
BOOL LIB_NC_GetStats(__reg("a6") struct NCBase *nb, __reg("a0") struct NCStats *stats)
{
    ULONG st;
    UBYTE *b;
    BOOL ok;

    if (stats && available(nb) && nb->x64)
    {
        ULONG regs[12] = {0};

        if (!nb->f_stats)
            return FALSE;
        regs[R_A1] = (ULONG)stats;
        regs[R_D1] = sizeof(*stats);
        return (BOOL)(nc_uni_call(nb->uni_base, nb->f_stats, regs) != 0);
    }

    if (!stats || !available(nb) || nb->hv_version < 8 || !(b = bounce(nb)))
        return FALSE;
    ObtainSemaphore(&nb->bounce_sem);
    request(nb, NC_OP_STATS, (ULONG)b, sizeof(*stats), 0, &st, NULL, NULL, 0);
    ok = st == NC_ST_OK;
    if (ok)
        CopyMem(b, stats, sizeof(*stats));
    ReleaseSemaphore(&nb->bounce_sem);
    return ok;
}

/* 1.7: the modules loaded right now, at most max of them; returns how many there are in all
   (may be more than max). info may be NULL with max 0. */
ULONG LIB_NC_ListModules(__reg("a6") struct NCBase *nb, __reg("a0") struct NCModuleInfo *info, __reg("d0") ULONG max)
{
    struct MinNode *n;
    ULONG count = 0;
    int i;

    ObtainSemaphoreShared(&nb->sem);
    for (n = nb->modules.mlh_Head; n->mln_Succ; n = n->mln_Succ)
    {
        struct NCModule *m = (struct NCModule *)n;

        if (info && count < max)
        {
            struct NCModuleInfo *mi = &info[count];

            mi->module = (ULONG)m;
            mi->size = m->size;
            mi->kernels = ((struct NativeHeader *)m->base)->entry_count;
            mi->calls = m->calls;
            mi->total_us = m->total_us;
            mi->timeout_us = m->timeout_us;
            mi->faults = m->faults;
            mi->jobs = 0;
            for (i = 0; i < NC_ASYNC_JOBS; i++)
                if (nb->job_module[i] == m)
                    mi->jobs++;
            for (i = 0; i < (int)sizeof(mi->owner); i++)
                mi->owner[i] = m->owner[i];
        }
        count++;
    }
    ReleaseSemaphore(&nb->sem);
    return count;
}

/* ---- Library management ---- */

static void cleanup(struct NCBase *nb)
{
    struct MinNode *n;
    int i;

    /* jobs first: they run code of the modules (1.8; before, modules were freed first) */
    for (i = 0; i < NC_ASYNC_JOBS; i++)
        if (nb->job_module[i])
            LIB_NC_CancelJob(nb, i + 1, NULL);
    while ((n = nb->modules.mlh_Head)->mln_Succ)
    {
        Remove((struct Node *)n);
        free_module(nb, (struct NCModule *)n);
    }
    if (nb->job_status_alloc)
        FreeMem(nb->job_status_alloc, NC_ASYNC_JOBS * sizeof(struct NCJobStatus) + 16);
    if (nb->block_alloc)
        FreeMem(nb->block_alloc, 2 * NC_BLOCK_SIZE);
    if (nb->bounce)
        FreeMem(nb->bounce, BOUNCE_SIZE);
    if (nb->uni_host)
    {
        /* the host DLL's thread pool ends, so that the DLL can be unloaded */
        if (nb->f_shutdown)
        {
            ULONG regs[12] = {0};

            nc_uni_call(nb->uni_base, nb->f_shutdown, regs);
        }
        uni_close_library(nb->uni_base, nb->uni_host);
    }
    if (nb->uni_base)
        CloseLibrary(nb->uni_base);
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
    InitSemaphore(&nb->bounce_sem);
    InitSemaphore(&nb->init_sem);
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
    if (nb->job_status)
        reap_jobs(nb);
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
    (APTR)LIB_NC_SetTimeout,
    (APTR)LIB_NC_ReadLog,
    (APTR)LIB_NC_GetFaultInfo,
    (APTR)LIB_NC_CancelJob,
    (APTR)LIB_NC_GetStats,
    (APTR)LIB_NC_ListModules,
    (APTR)-1
};

const ULONG InitTable[4] =
{
    sizeof(struct NCBase),
    (ULONG)FuncTable,
    0,
    (ULONG)LibInit
};
