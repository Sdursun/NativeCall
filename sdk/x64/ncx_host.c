/*
    ncx_host.c - nativecall-windows-x86-64.dll: runs x64 kernel modules for nativecall.library
    under WinUAE (x64/README.md). Installed once in WinUAE's plugins folder; the library opens it
    through uaenative.library (native_code=true) when there is no hypervisor.

    A module (<prefix>.nx64, made by "nc make nx64") is a header and a DLL holding the kernels.
    The library reads it on the Amiga and hands it over with ncx_load: the DLL is written to
    %TEMP%\nativecall\ under a name of its own and loaded from there, so every load is a separate
    instance with its own static variables, as on the Pi.

    Functions, called with uaenative's call_function (d1-d7/a1-a5 in struct uni, d0 = result).
    All values the Amiga reads from memory are big-endian.

      ncx_version ()                          -> NCX_HOST_VERSION
      ncx_cores   ()                          -> logical processors of the PC
      ncx_now     ()                          -> the PC's clock, microseconds (low 32 bits)
      ncx_check   (a1 = address, d1 = size)   -> 1 if the block is reachable as host memory
      ncx_load    (a1 = .nx64 image, d1 = size) -> module handle (1..), 0 on failure
      ncx_unload  (d1 = handle)
      ncx_call    (d1 = handle, d2 = kernel, a1 = params, a2 = out)
                  -> the kernel's result; it runs once on the emulation thread with the params
                     in place (it may write them), as nc_call / one piece of nc_run
      ncx_run     (d1 = handle, d2 = kernel, d3 = from, d4 = to, d5 = chunk, d6 = params size,
                   d7 = threads (0: all), a1 = params, a2 = out)
                  -> the sum of the results; nc_run_mp: [from, to) in pieces of chunk units,
                     handed out from a shared counter, each call on its own copy of the params
                     with the piece in its NCRange; the params are not written
      out (a2, may be 0): 4 longs: status (NC_ST_*), host microseconds, threads used, 0
      ncx_start, ncx_poll, ncx_wait, ncx_cancel: asynchronous jobs (nc_start_mp), see below
      ncx_shutdown ()                         -> ends the thread pool (library expunge)

    The emulation stands still while a call runs (call_function is synchronous): the Amiga's
    clocks do not see that time, the microseconds in out[] do.
*/

#include <windows.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <malloc.h>

#define EXPORT __declspec(dllexport)

#define NCX_HOST_VERSION    2       /* 2: faults, time limit, log, statistics */

/* nc_abi.h */
#define NC_ST_OK            0x01
#define NC_ST_BADENTRY      0x82

/* .nx64 file header (big-endian), sdk/tools/mknx64.py */
#define NX64_MAGIC          0x4e583634u     /* 'NX64' */
#define NX64_VERSION        1
#define NX64_HEADER         32

/* the module DLL's export (nativecall/kernel.h, x64 branch) */
typedef uint32_t (*NCKernelFunc)(void *params);
struct NCX64Module
{
    uint32_t magic;
    uint32_t version;
    uint32_t count;
    const NCKernelFunc *entries;
    char **membase;
    void (**log)(const char *text, uint32_t len);   /* version 2 */
};
#define NCX64_MODULE_MAGIC      0x4e584d44u
#define NCX64_MODULE_VERSION    2

#define MAX_MODULES     64
#define MAX_PARAMS      1024
#define MAX_THREADS     64

struct uni
{
    int32_t d1, d2, d3, d4, d5, d6, d7;
    int32_t a1, a2, a3, a4, a5;
    int32_t a7;
};

typedef void *(__cdecl *uni_resolve_function)(uint32_t ptr);
EXPORT uni_resolve_function uni_resolve = 0;   /* set by WinUAE */

struct module
{
    HMODULE dll;
    const struct NCX64Module *desc;
    char path[MAX_PATH];
};

static struct module modules[MAX_MODULES];
static unsigned load_counter;
static CRITICAL_SECTION lock;

static void __cdecl host_log(const char *text, uint32_t len);

static uint32_t be32(uint32_t v) { return __builtin_bswap32(v); }

static uint32_t get32(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static uint64_t now_us(void)
{
    static LARGE_INTEGER f;
    LARGE_INTEGER t;
    if (!f.QuadPart)
        QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (uint64_t)(t.QuadPart / f.QuadPart) * 1000000u + (uint64_t)(t.QuadPart % f.QuadPart) * 1000000u / f.QuadPart;
}

/* Threads for nc_run_mp: the logical processors less one, which stays for WinUAE's own threads
   (display, sound, input). With all of them busy, WinUAE's threads waited and single calls took
   up to 6 ms instead of 0.3. */
static int cores(void)
{
    int n = (int)GetActiveProcessorCount(ALL_PROCESSOR_GROUPS) - 1;
    return n < 1 ? 1 : n > MAX_THREADS ? MAX_THREADS : n;
}

/* Host address of Amiga address 0 for the block [addr, addr + size): NULL unless both ends are
   at the same distance (one block, as with WinUAE's direct memory) */
static char *membase(uint32_t addr, uint32_t size)
{
    char *first, *last;
    if (!uni_resolve || !size || addr + size - 1 < addr)
        return NULL;
    first = uni_resolve(addr);
    last = uni_resolve(addr + size - 1);
    if (!first || !last || last - first != (ptrdiff_t)(size - 1))
        return NULL;
    return first - addr;
}

static void temp_dir(char *dir)
{
    DWORD n = GetTempPathA(MAX_PATH - 32, dir);
    if (n == 0 || n > MAX_PATH - 32)
        strcpy(dir, ".\\");
    strcat(dir, "nativecall");
    CreateDirectoryA(dir, NULL);
    strcat(dir, "\\");
}

/* Files of earlier runs (WinUAE ended while modules were loaded): those still in use by another
   WinUAE cannot be deleted and stay */
static void clean_temp(void)
{
    char dir[MAX_PATH], pattern[MAX_PATH], file[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE h;

    temp_dir(dir);
    snprintf(pattern, sizeof(pattern), "%sm*.dll", dir);
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do
    {
        snprintf(file, sizeof(file), "%s%s", dir, fd.cFileName);
        DeleteFileA(file);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

EXPORT int __cdecl uni_init(void)
{
    InitializeCriticalSection(&lock);
    clean_temp();
    return 0;
}

static void dump_ring(const char *why);
static int debugging(void);
static FILE *debug_log;

EXPORT int32_t __cdecl ncx_version(struct uni *u)
{
    (void)u;
    if (debugging())
    {
        fprintf(debug_log, "ncx_version: library opened the DLL\n");
        fflush(debug_log);
    }
    dump_ring("library opened the DLL (again)");
    return NCX_HOST_VERSION;
}

EXPORT int32_t __cdecl ncx_cores(struct uni *u)
{
    (void)u;
    return cores();
}

EXPORT int32_t __cdecl ncx_now(struct uni *u)
{
    (void)u;
    return (int32_t)(uint32_t)now_us();
}

EXPORT int32_t __cdecl ncx_check(struct uni *u)
{
    return membase((uint32_t)u->a1, (uint32_t)u->d1) != NULL;
}

EXPORT int32_t __cdecl ncx_load(struct uni *u)
{
    uint32_t addr = (uint32_t)u->a1, size = (uint32_t)u->d1, dll_size, count;
    const unsigned char *image;
    char *base, dir[MAX_PATH];
    struct module *m = NULL;
    HANDLE fh;
    DWORD written;
    int i, handle = 0;

    if (size < NX64_HEADER || !(base = membase(addr, size)))
        return 0;
    image = (const unsigned char *)base + addr;
    dll_size = get32(image + 20);
    count = get32(image + 12);
    if (get32(image) != NX64_MAGIC || get32(image + 4) != NX64_VERSION || get32(image + 16) != NX64_HEADER
        || dll_size > size - NX64_HEADER || count == 0)
        return 0;

    EnterCriticalSection(&lock);
    for (i = 0; i < MAX_MODULES; i++)
        if (!modules[i].dll)
        {
            m = &modules[i];
            handle = i + 1;
            break;
        }
    if (m)
    {
        temp_dir(dir);
        snprintf(m->path, sizeof(m->path), "%sm%08x_%lu_%u.dll", dir, (unsigned)get32(image + 8),
                 (unsigned long)GetCurrentProcessId(), ++load_counter);
        fh = CreateFileA(m->path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (fh == INVALID_HANDLE_VALUE)
            m = NULL;
        else
        {
            BOOL ok = WriteFile(fh, image + NX64_HEADER, dll_size, &written, NULL) && written == dll_size;
            CloseHandle(fh);
            m->dll = ok ? LoadLibraryA(m->path) : NULL;
            m->desc = m->dll ? (const struct NCX64Module *)GetProcAddress(m->dll, "ncx_module") : NULL;
            if (!m->desc || m->desc->magic != NCX64_MODULE_MAGIC || (m->desc->version < 1 || m->desc->version > NCX64_MODULE_VERSION)
                || m->desc->count != count || !m->desc->entries || !m->desc->membase)
            {
                if (m->dll)
                    FreeLibrary(m->dll);
                DeleteFileA(m->path);
                m->dll = NULL;
                m->desc = NULL;
                m = NULL;
            }
            else if (m->desc->version >= 2 && m->desc->log)
                *m->desc->log = host_log;
        }
    }
    LeaveCriticalSection(&lock);
    return m ? handle : 0;
}

EXPORT int32_t __cdecl ncx_unload(struct uni *u)
{
    uint32_t handle = (uint32_t)u->d1;
    struct module *m;

    if (handle < 1 || handle > MAX_MODULES)
        return 0;
    EnterCriticalSection(&lock);
    m = &modules[handle - 1];
    if (m->dll)
    {
        FreeLibrary(m->dll);
        DeleteFileA(m->path);
        m->dll = NULL;
        m->desc = NULL;
    }
    LeaveCriticalSection(&lock);
    return 1;
}

static void put_out(uint32_t addr, uint32_t status, uint64_t us, uint32_t threads)
{
    uint32_t *out;
    char *base;

    if (!addr || !(base = membase(addr, 16)))
        return;
    out = (uint32_t *)(base + addr);
    out[0] = be32(status);
    out[1] = be32(us > 0xffffffffu ? 0xffffffffu : (uint32_t)us);
    out[2] = be32(threads);
    out[3] = 0;
}

/* The module and kernel of a call, its memory base set; NULL if invalid */
static const struct NCX64Module *kernel_of(uint32_t handle, uint32_t index, char *base)
{
    const struct NCX64Module *d;

    if (handle < 1 || handle > MAX_MODULES || !(d = modules[handle - 1].desc) || index >= d->count)
        return NULL;
    *d->membase = base;
    return d;
}

/*
    Faults, time limit, log and statistics (host version 2).

    Every kernel call runs "guarded": __builtin_setjmp keeps a way back. A vectored exception
    handler that sees an access violation, an illegal instruction, a division by zero or a stack
    overflow while the thread runs a kernel records the fault and lets the thread continue at
    fault_return(), which jumps back: the call ends with NC_ST_FAULT and WinUAE goes on. A
    watchdog thread stops a kernel that runs past its time limit the same way (NC_ST_TIMEOUT):
    it suspends the thread, and only if it is executing code of a kernel module (not inside the
    C library or Windows, which may hold locks) moves it to fault_return(); otherwise it tries
    again on its next round. A kernel stopped like this may leave its output half written.

    Kernels log through the module's nc_x64_logfn (kernel.h, module version 2) into a ring of
    NC_LOG_SIZE bytes; ncx_logread copies it out. ncx_stats reports the time threads spent in
    kernels: the emulation thread as "core" 0, the workers together as "core" 1.
*/
#define NC_ST_FAULT         0x84
#define NC_ST_TIMEOUT       0x87
#define NC_FEAT_TIMEOUT     0x1
#define NC_FEAT_X64         0x80000000u     /* nc_abi.h: the kernels run on the PC (WinUAE) */
#define DEFAULT_TIMEOUT_US  1000000u
#define LOG_SIZE            16384u
#define FAULT_FRAMES        10
#define NOOFFSET            0xffffffffu
#define WATCH_MS            10

struct guard
{
    void *jb[5];                /* __builtin_setjmp: [0] frame, [1] resume, [2] stack */
    volatile int code;          /* 0 or NC_ST_FAULT / NC_ST_TIMEOUT once stopped */
    volatile uint64_t deadline; /* host microseconds */
    HANDLE thread;
};

/* thread 0 = the emulation thread, 1.. = workers */
#define GUARD_THREADS   (MAX_THREADS + 1)
static __thread struct guard *tls_guard;
static __thread HANDLE tls_thread;
static __thread int tls_index;
static struct guard *volatile active[GUARD_THREADS];
static volatile LONG64 busy_us[GUARD_THREADS];
static volatile LONG calls_n[GUARD_THREADS];

static SRWLOCK fault_lock = SRWLOCK_INIT;
static struct
{
    uint32_t status, thread, count, code;
    uint64_t fault_addr, rip, rsp, regs[16];
    uint32_t backtrace[FAULT_FRAMES];
} fault_rec;

static SRWLOCK log_lock = SRWLOCK_INIT;
static char log_ring[LOG_SIZE];
static volatile uint32_t log_total;    /* bytes ever written */

static void log_append(const char *text, uint32_t len)
{
    uint32_t i;

    AcquireSRWLockExclusive(&log_lock);
    for (i = 0; i < len; i++)
        log_ring[(log_total + i) % LOG_SIZE] = text[i];
    log_total += len;
    ReleaseSRWLockExclusive(&log_lock);
}

/* what kernels call (module version 2): at most 255 bytes a call */
static void __cdecl host_log(const char *text, uint32_t len)
{
    log_append(text, len > 255 ? 255 : len);
}

static void log_line(const char *format, ...)
{
    char b[256];
    va_list a;
    int n;

    va_start(a, format);
    n = vsnprintf(b, sizeof(b), format, a);
    va_end(a);
    if (n > (int)sizeof(b) - 1)
        n = sizeof(b) - 1;
    if (n > 0)
        log_append(b, (uint32_t)n);
}

/* The module DLL holding an address: its slot, or -1 */
static int module_of(uint64_t address)
{
    HMODULE h = NULL;
    int i;

    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)(uintptr_t)address, &h))
        return -1;
    for (i = 0; i < MAX_MODULES; i++)
        if (modules[i].dll && modules[i].dll == h)
            return i;
    return -1;
}

static uint32_t module_offset(uint64_t address)
{
    int i = module_of(address);
    return i < 0 ? NOOFFSET : (uint32_t)(address - (uint64_t)(uintptr_t)modules[i].dll);
}

/* Fault details and the backtrace inside the kernel module. GCC for Windows x64 does not keep
   a frame pointer chain (rbp points into the frame, not at the saved rbp), so the frames are
   unwound with the module's own unwind tables (.pdata), as a debugger does. A function without
   an entry there (a leaf, ___chkstk_ms) has its return address at rsp. */
static void record_fault(int thread, int code, uint32_t exception, uint64_t fault_addr, const CONTEXT *c, ULONG_PTR stack_lo,
                         ULONG_PTR stack_hi)
{
    static CONTEXT u;           /* under fault_lock; not on the stack, which may have overflowed */
    int i, ended = 0;

    AcquireSRWLockExclusive(&fault_lock);
    u = *c;
    fault_rec.status = (uint32_t)code;
    fault_rec.thread = (uint32_t)thread;
    fault_rec.count++;
    fault_rec.code = exception;
    fault_rec.fault_addr = fault_addr;
    fault_rec.rip = c->Rip;
    fault_rec.rsp = c->Rsp;
    fault_rec.regs[0] = c->Rax; fault_rec.regs[1] = c->Rcx; fault_rec.regs[2] = c->Rdx; fault_rec.regs[3] = c->Rbx;
    fault_rec.regs[4] = c->Rsp; fault_rec.regs[5] = c->Rbp; fault_rec.regs[6] = c->Rsi; fault_rec.regs[7] = c->Rdi;
    fault_rec.regs[8] = c->R8; fault_rec.regs[9] = c->R9; fault_rec.regs[10] = c->R10; fault_rec.regs[11] = c->R11;
    fault_rec.regs[12] = c->R12; fault_rec.regs[13] = c->R13; fault_rec.regs[14] = c->R14; fault_rec.regs[15] = c->R15;
    fault_rec.backtrace[0] = module_offset(c->Rip);
    for (i = 1; i < FAULT_FRAMES; i++)
    {
        DWORD64 image = 0;
        PRUNTIME_FUNCTION f;

        /* only frames of the kernel module: the caller's frames are the host DLL's and WinUAE's */
        if (!ended && module_of(u.Rip) >= 0 && u.Rsp >= stack_lo && u.Rsp + 8 <= stack_hi && !(u.Rsp & 7))
        {
            f = RtlLookupFunctionEntry(u.Rip, &image, NULL);
            if (f)
            {
                PVOID handler_data;
                DWORD64 establisher;

                RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, u.Rip, f, &u, &handler_data, &establisher, NULL);
            }
            else
            {
                u.Rip = *(const DWORD64 *)(uintptr_t)u.Rsp;
                u.Rsp += 8;
            }
            fault_rec.backtrace[i] = module_offset(u.Rip);
            if (fault_rec.backtrace[i] == NOOFFSET)
                ended = 1;
        }
        else
        {
            ended = 1;
            fault_rec.backtrace[i] = NOOFFSET;
        }
    }
    ReleaseSRWLockExclusive(&fault_lock);
    if (code == NC_ST_TIMEOUT)
        log_line("x64: time limit, thread %d stopped at module offset 0x%x\n", thread, fault_rec.backtrace[0]);
    else
        log_line("x64: fault 0x%08x at module offset 0x%x, address 0x%llx, thread %d\n", (unsigned)exception,
                 fault_rec.backtrace[0], (unsigned long long)fault_addr, thread);
}

static void __attribute__((noreturn, used)) fault_return(void)
{
    __builtin_longjmp(tls_guard->jb, 1);
}

/* Continue the thread at fault_return(), on a stack just below the guarded call's frame */
static void redirect(CONTEXT *c, const struct guard *g)
{
    c->Rsp = (((uint64_t)(uintptr_t)g->jb[2] - 1024) & ~(uint64_t)15) - 8;
    c->Rip = (uint64_t)(uintptr_t)fault_return;
}

static LONG WINAPI guard_handler(EXCEPTION_POINTERS *e)
{
    struct guard *g = tls_guard;
    DWORD code = e->ExceptionRecord->ExceptionCode;
    ULONG_PTR lo, hi;
    uint64_t fault_addr = 0;

    if (!g || g->code)
        return EXCEPTION_CONTINUE_SEARCH;
    switch (code)
    {
        case EXCEPTION_ACCESS_VIOLATION:
        case EXCEPTION_IN_PAGE_ERROR:
            fault_addr = e->ExceptionRecord->ExceptionInformation[1];
            break;
        case EXCEPTION_ILLEGAL_INSTRUCTION:
        case EXCEPTION_PRIV_INSTRUCTION:
        case EXCEPTION_INT_DIVIDE_BY_ZERO:
        case EXCEPTION_INT_OVERFLOW:
        case EXCEPTION_STACK_OVERFLOW:
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
        case EXCEPTION_DATATYPE_MISALIGNMENT:
            break;
        default:
            return EXCEPTION_CONTINUE_SEARCH;
    }
    g->code = NC_ST_FAULT;
    GetCurrentThreadStackLimits(&lo, &hi);
    record_fault(tls_index, NC_ST_FAULT, (uint32_t)code, fault_addr, e->ContextRecord, lo, hi);
    redirect(e->ContextRecord, g);
    return EXCEPTION_CONTINUE_EXECUTION;
}

static HANDLE watchdog;
static volatile LONG watchdog_quit;
static PVOID guard_veh;
static PVOID debug_veh;            /* diagnostics (below) */

/* Stops guarded calls past their deadline: only while they run code of a kernel module */
static DWORD WINAPI watchdog_main(LPVOID arg)
{
    HMODULE self = NULL;
    int i;

    (void)arg;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCSTR)(void *)watchdog_main, &self);
    while (!watchdog_quit)
    {
        uint64_t now;

        Sleep(WATCH_MS);
        now = now_us();
        for (i = 0; i < GUARD_THREADS; i++)
        {
            struct guard *g = active[i];
            CONTEXT c;

            if (!g || g->code || now < g->deadline)
                continue;
            if (SuspendThread(g->thread) == (DWORD)-1)
                continue;
            c.ContextFlags = CONTEXT_FULL;
            /* still in the same call, not stopped yet, and in a kernel's own code */
            if (active[i] == g && !g->code && GetThreadContext(g->thread, &c) && module_of(c.Rip) >= 0)
            {
                ULONG_PTR lo = c.Rsp - 0x100000, hi = c.Rsp + 0x100000;

                g->code = NC_ST_TIMEOUT;
                record_fault(i, NC_ST_TIMEOUT, 0, 0, &c, lo, hi);
                redirect(&c, g);
                SetThreadContext(g->thread, &c);
            }
            ResumeThread(g->thread);
        }
    }
    if (self)
        FreeLibraryAndExitThread(self, 0);
    return 0;
}

static volatile LONG guards_started;

static void guards_start(void)
{
    if (InterlockedCompareExchange(&guards_started, 1, 0) == 0)
    {
        HANDLE h;

        if (!guard_veh)
            guard_veh = AddVectoredExceptionHandler(1, guard_handler);
        h = CreateThread(NULL, 0, watchdog_main, NULL, 0, NULL);
        watchdog = h;
    }
}

/* Runs one kernel call guarded; *code: 0, NC_ST_FAULT or NC_ST_TIMEOUT */
static uint32_t guarded(NCKernelFunc kernel, void *params, uint32_t timeout_us, int *code)
{
    struct guard g;
    uint64_t t0;
    uint32_t r;
    int index = tls_index;

    guards_start();
    if (!tls_thread)
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &tls_thread, 0, FALSE,
                        DUPLICATE_SAME_ACCESS);
    g.code = 0;
    g.thread = tls_thread;
    t0 = now_us();
    g.deadline = t0 + (timeout_us ? timeout_us : DEFAULT_TIMEOUT_US);
    if (__builtin_setjmp(g.jb))
    {
        /* back from fault_return() */
        active[index] = NULL;
        tls_guard = NULL;
        if (fault_rec.code == EXCEPTION_STACK_OVERFLOW)
            _resetstkoflw();
        InterlockedExchangeAdd64(&busy_us[index], (LONG64)(now_us() - t0));
        *code = g.code;
        return 0;
    }
    tls_guard = &g;
    active[index] = &g;
    r = kernel(params);
    active[index] = NULL;
    tls_guard = NULL;
    InterlockedExchangeAdd64(&busy_us[index], (LONG64)(now_us() - t0));
    InterlockedIncrement(&calls_n[index]);
    *code = 0;
    return r;
}

__attribute__((destructor)) static void guards_end(void)
{
    /* the handlers must not outlive the DLL */
    if (guard_veh)
        RemoveVectoredExceptionHandler(guard_veh);
    guard_veh = NULL;
    if (debug_veh)
        RemoveVectoredExceptionHandler(debug_veh);
    debug_veh = NULL;
}

static void put32(char *p, uint32_t v)
{
    uint32_t b = be32(v);
    memcpy(p, &b, 4);
}

/* ncx_features () -> NC_FEAT_* */
EXPORT int32_t __cdecl ncx_features(struct uni *u)
{
    (void)u;
    return (int32_t)(NC_FEAT_TIMEOUT | NC_FEAT_X64);
}

/* ncx_logread (a1 = buffer, d1 = size, d2 = position, a2 = 2 longs out: new position, bytes lost)
   -> bytes copied */
EXPORT int32_t __cdecl ncx_logread(struct uni *u)
{
    uint32_t addr = (uint32_t)u->a1, size = (uint32_t)u->d1, pos = (uint32_t)u->d2, n = 0, lost = 0, total, i;
    char *base, *out;

    if (!size || !(base = membase(addr, size)))
        return 0;
    AcquireSRWLockShared(&log_lock);
    total = log_total;
    if (total - pos > LOG_SIZE)
    {
        lost = total - LOG_SIZE - pos;
        pos = total - LOG_SIZE;
    }
    for (i = 0; pos + i != total && i < size; i++)
        base[addr + i] = log_ring[(pos + i) % LOG_SIZE];
    n = i;
    ReleaseSRWLockShared(&log_lock);
    if (u->a2 && (out = membase((uint32_t)u->a2, 8)))
    {
        put32(out + (uint32_t)u->a2, pos + n);
        put32(out + (uint32_t)u->a2 + 4, lost);
    }
    return (int32_t)n;
}

/* ncx_faultinfo (a1 = struct NCFaultInfo, d1 = its size) -> 1 if there was a fault. The PC's
   registers go to x[0..15] (rax rcx rdx rbx rsp rbp rsi rdi r8..r15), "esr" is the Windows
   exception code (0 for a time-out), "core" the thread (0 = the emulation thread). */
EXPORT int32_t __cdecl ncx_faultinfo(struct uni *u)
{
    uint32_t addr = (uint32_t)u->a1, size = (uint32_t)u->d1;
    char buf[4 * (12 + 62 + FAULT_FRAMES)], *base;
    int i;

    if (size > sizeof(buf))
        size = sizeof(buf);
    if (!size || !(base = membase(addr, size)))
        return 0;
    memset(buf, 0, sizeof(buf));
    AcquireSRWLockShared(&fault_lock);
    put32(buf + 0, 1);
    put32(buf + 4, fault_rec.status);
    put32(buf + 8, fault_rec.thread);
    put32(buf + 12, 0);
    put32(buf + 16, fault_rec.count);
    put32(buf + 20, fault_rec.code);
    put32(buf + 24, (uint32_t)(fault_rec.fault_addr >> 32));
    put32(buf + 28, (uint32_t)fault_rec.fault_addr);
    put32(buf + 32, (uint32_t)(fault_rec.rip >> 32));
    put32(buf + 36, (uint32_t)fault_rec.rip);
    put32(buf + 40, (uint32_t)(fault_rec.rsp >> 32));
    put32(buf + 44, (uint32_t)fault_rec.rsp);
    for (i = 0; i < 16; i++)
    {
        put32(buf + 48 + 8 * i, (uint32_t)(fault_rec.regs[i] >> 32));
        put32(buf + 52 + 8 * i, (uint32_t)fault_rec.regs[i]);
    }
    for (i = 0; i < FAULT_FRAMES; i++)
        put32(buf + 48 + 8 * 31 + 4 * i, fault_rec.backtrace[i]);
    i = fault_rec.status != 0;
    ReleaseSRWLockShared(&fault_lock);
    memcpy(base + addr, buf, size);
    return i;
}

static int async_jobs_running(void);

/* ncx_stats (a1 = struct NCStats, d1 = its size) -> 1. "Core" 0 = the emulation thread, 1 = the
   workers together (their time divided by their number), 2 and 3 absent; clock in microseconds. */
EXPORT int32_t __cdecl ncx_stats(struct uni *u)
{
    uint32_t addr = (uint32_t)u->a1, size = (uint32_t)u->d1;
    char buf[32 + 16 * 4 + 8], *base;
    uint64_t now = now_us(), w = 0;
    uint32_t wc = 0;
    int i, workers = cores() - 1;

    if (size > sizeof(buf))
        size = sizeof(buf);
    if (!size || !(base = membase(addr, size)))
        return 0;
    memset(buf, 0, sizeof(buf));
    put32(buf + 0, 1);
    put32(buf + 4, 1000000);
    put32(buf + 8, (uint32_t)(now >> 32));
    put32(buf + 12, (uint32_t)now);
    put32(buf + 16, 0);
    put32(buf + 20, (uint32_t)async_jobs_running());
    put32(buf + 24, fault_rec.count);
    put32(buf + 28, log_total);
    put32(buf + 32, (uint32_t)((uint64_t)busy_us[0] >> 32));
    put32(buf + 36, (uint32_t)busy_us[0]);
    put32(buf + 40, (uint32_t)calls_n[0]);
    put32(buf + 44, 0x1);                               /* NC_CORE_NATIVE */
    for (i = 1; i < GUARD_THREADS; i++)
    {
        w += (uint64_t)busy_us[i];
        wc += (uint32_t)calls_n[i];
    }
    if (workers > 0)
    {
        w /= (uint64_t)workers;
        put32(buf + 48, (uint32_t)(w >> 32));
        put32(buf + 52, (uint32_t)w);
        put32(buf + 56, wc);
        put32(buf + 60, 0x1 | 0x2);                     /* NC_CORE_NATIVE | NC_CORE_HELPER */
    }
    put32(buf + 96, 0);                                 /* hv_version: none */
    put32(buf + 100, NC_FEAT_TIMEOUT | NC_FEAT_X64);
    memcpy(base + addr, buf, size);
    return 1;
}

/*
    Diagnostics, only when %TEMP%\nativecall\debug exists: around every ncx_call / ncx_run the
    Amiga's first KB (exception vectors, ExecBase pointer) is compared before and after; the 68k
    stands still meanwhile, so a change there was written by a kernel. It is logged to
    %TEMP%\nativecall\ncx.log with the kernel and its params.
*/
#define LOW_SIZE    1024
static int debug_on = -1;
static FILE *debug_log;
static unsigned char low_before[LOW_SIZE];

static LONG WINAPI crash_handler(EXCEPTION_POINTERS *e);

static int debugging(void)
{
    char p[MAX_PATH];

    if (debug_on < 0)
    {
        temp_dir(p);
        strcat(p, "debug");
        debug_on = GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES;
        if (debug_on)
        {
            temp_dir(p);
            strcat(p, "ncx.log");
            debug_log = fopen(p, "a");
            if (debug_log)
            {
                fprintf(debug_log, "--- ncx_host diagnostics on\n");
                fflush(debug_log);
                debug_veh = AddVectoredExceptionHandler(1, crash_handler);
            }
        }
    }
    return debug_on && debug_log;
}

/* The last call (diagnostics): written out when a kernel crashes */
static struct
{
    const char *what;
    uint32_t handle, kernel, params, size, from, to;
    char *membase;
    unsigned char bytes[64];
} last_call;

static LONG WINAPI crash_handler(EXCEPTION_POINTERS *e)
{
    HMODULE m = NULL;
    char name[MAX_PATH] = "?";
    int i;

    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || !debug_log
        || !GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)e->ExceptionRecord->ExceptionAddress, &m))
        return EXCEPTION_CONTINUE_SEARCH;
    for (i = 0; i < MAX_MODULES; i++)
        if (modules[i].dll == m)
            break;
    if (i == MAX_MODULES)
        return EXCEPTION_CONTINUE_SEARCH;       /* not a kernel module: WinUAE's own (JIT) */
    GetModuleFileNameA(m, name, sizeof(name));
    fprintf(debug_log, "CRASH thread %lu in %s+0x%llx, %s of $%llx (Amiga $%08llx)\n", GetCurrentThreadId(), name,
            (unsigned long long)((char *)e->ExceptionRecord->ExceptionAddress - (char *)m),
            e->ExceptionRecord->ExceptionInformation[0] ? "write" : "read",
            (unsigned long long)e->ExceptionRecord->ExceptionInformation[1],
            (unsigned long long)((char *)e->ExceptionRecord->ExceptionInformation[1] - last_call.membase));
    fprintf(debug_log, "  last call %s module %u kernel %u params $%08x size %u range %u..%u membase %p\n  params:",
            last_call.what, (unsigned)last_call.handle, (unsigned)last_call.kernel, (unsigned)last_call.params,
            (unsigned)last_call.size, (unsigned)last_call.from, (unsigned)last_call.to, (void *)last_call.membase);
    for (i = 0; i < 64 && i < (int)last_call.size; i++)
        fprintf(debug_log, "%s%02x", i % 4 ? "" : " ", last_call.bytes[i]);
    fprintf(debug_log, "\n");
    fflush(debug_log);
    dump_ring("CRASH");
    return EXCEPTION_CONTINUE_SEARCH;
}

/* The last calls (diagnostics): written out when a kernel crashes and when the library opens the
   DLL again (after an Amiga reset), so the calls before a reset can be read */
#define RING 4096
static struct ring_entry
{
    char what;                  /* 'c' ncx_call, 'r' ncx_run */
    uint32_t handle, kernel, params, from, to;
    uint64_t us;
    unsigned char bytes[32];
} ring[RING];
static unsigned ring_n, ring_dumped;

static void dump_ring(const char *why)
{
    unsigned i, first;
    int k;

    if (!debug_log || ring_n == ring_dumped)
        return;
    first = ring_n - ring_dumped > 40 ? ring_n - 40 : ring_dumped;
    fprintf(debug_log, "%s: last %u of %u calls\n", why, ring_n - first, ring_n);
    for (i = first; i < ring_n; i++)
    {
        const struct ring_entry *r = &ring[i % RING];

        fprintf(debug_log, "  %llu us %c mod %u k %u params $%08x %u..%u:", (unsigned long long)r->us, r->what,
                (unsigned)r->handle, (unsigned)r->kernel, (unsigned)r->params, (unsigned)r->from, (unsigned)r->to);
        for (k = 0; k < 32; k++)
            fprintf(debug_log, "%s%02x", k % 4 ? "" : " ", r->bytes[k]);
        fprintf(debug_log, "\n");
    }
    fflush(debug_log);
    ring_dumped = ring_n;
}

/* WinUAE unloads the DLL at an Amiga reset when nothing else holds it */
__attribute__((destructor)) static void dump_at_unload(void)
{
    dump_ring("DLL unloaded");
}

static void note_call(const char *what, uint32_t handle, uint32_t kernel, uint32_t params, uint32_t size,
                      uint32_t from, uint32_t to)
{
    struct ring_entry *r;
    char *b;

    if (!debugging())
        return;
    r = &ring[ring_n++ % RING];
    r->what = what[4];
    r->handle = handle;
    r->kernel = kernel;
    r->params = params;
    r->from = from;
    r->to = to;
    r->us = now_us();
    if ((b = membase(params, 32)))
        memcpy(r->bytes, b + params, 32);
    last_call.what = what;
    last_call.handle = handle;
    last_call.kernel = kernel;
    last_call.params = params;
    last_call.size = size;
    last_call.from = from;
    last_call.to = to;
    b = membase(params, size);
    last_call.membase = b;
    if (b)
        memcpy(last_call.bytes, b + params, size < 64 ? size : 64);
}

static void low_snap(void)
{
    char *b;

    if (debugging() && (b = membase(0, LOW_SIZE)))
        memcpy(low_before, b, LOW_SIZE);
}

static void low_check(const char *what, uint32_t handle, uint32_t kernel, uint32_t params, uint32_t size)
{
    char *b;
    const unsigned char *now, *pp;
    int i, n = 0;
    uint32_t k;

    if (!debugging() || !(b = membase(0, LOW_SIZE)))
        return;
    now = (const unsigned char *)b;
    for (i = 0; i < LOW_SIZE; i++)
        if (now[i] != low_before[i])
        {
            if (n == 0)
            {
                fprintf(debug_log, "%s module %u kernel %u params $%08x: low memory changed\n", what,
                        (unsigned)handle, (unsigned)kernel, (unsigned)params);
                pp = (const unsigned char *)(membase(params, size) ? b + params : NULL);
                if (pp)
                {
                    fprintf(debug_log, "  params:");
                    for (k = 0; k < size && k < 64; k++)
                        fprintf(debug_log, "%s%02x", k % 4 ? "" : " ", pp[k]);
                    fprintf(debug_log, "\n");
                }
            }
            if (n < 16)
                fprintf(debug_log, "  $%03x: %02x -> %02x\n", i, low_before[i], now[i]);
            n++;
        }
    if (n)
    {
        fprintf(debug_log, "  %d bytes changed\n", n);
        fflush(debug_log);
    }
}

EXPORT int32_t __cdecl ncx_call(struct uni *u)
{
    uint32_t params = (uint32_t)u->a1, result;
    const struct NCX64Module *d;
    uint64_t t0 = now_us();
    char *base;
    int code;

    /* the params must be reachable; 8 bytes is all a call can be sure of */
    if (!(base = membase(params, 8)) || !(d = kernel_of((uint32_t)u->d1, (uint32_t)u->d2, base)))
    {
        put_out((uint32_t)u->a2, NC_ST_BADENTRY, 0, 0);
        return -1;
    }
    note_call("ncx_call", (uint32_t)u->d1, (uint32_t)u->d2, params, 64, 0, 0);
    low_snap();
    result = guarded(d->entries[(uint32_t)u->d2], base + params, (uint32_t)u->d3, &code);
    low_check("ncx_call", (uint32_t)u->d1, (uint32_t)u->d2, params, 64);
    put_out((uint32_t)u->a2, code ? (uint32_t)code : NC_ST_OK, now_us() - t0, 1);
    return (int32_t)result;
}


/*
    Jobs and the thread pool.

    Slot 0 is the job of ncx_run (the emulation thread waits for it and works on it too); slots
    1..MAX_ASYNC are jobs of ncx_start, which run on the workers while the emulation goes on.
    A job is a range [next, to) handed out in pieces of chunk units from a shared counter; each
    thread taking part works on its own copy of the params with its piece in the NCRange. The job
    keeps a copy of the params made when it was published, so the Amiga may not even change them
    by mistake while it runs.

    Workers: cores() - 1, started by the first ncx_run or ncx_start that needs them. Under
    pool_lock a worker picks a job with pieces left (slot 0 first), counts itself in its active
    count, and runs pieces until there are none (or the job is cancelled); then it leaves it
    under the lock again. The thread that leaves an exhausted job last marks it done. With no
    work a worker waits SPIN_US without sleeping (NC_Run sends its pieces back to back and waking
    a sleeping thread costs tens of microseconds), then sleeps on pool_cv.

    Each worker holds a reference to this DLL, so it is not unloaded under them; ncx_shutdown
    (nativecall.library calls it when it is expunged) ends them, and the last one lets the DLL go
    (FreeLibraryAndExitThread). After an Amiga reset without that the DLL stays loaded until
    WinUAE ends; an asynchronous job running at the reset runs to its end and writes the memory
    of the new boot: keep jobs short.
*/
#define SPIN_US     100
#define MAX_ASYNC   8
#define NC_ST_RUNNING   0x02
#define NC_ST_BADJOB    0x86
#define NC_ST_CANCELLED 0x88

struct job
{
    int used, async;
    NCKernelFunc kernel;
    uint32_t size, to, chunk;
    volatile LONGLONG next;
    volatile LONG sum;
    volatile LONG active;       /* threads in it (under pool_lock) */
    volatile LONG cancel;
    volatile LONG done;         /* asynchronous: nothing runs any more */
    volatile LONG status;       /* NC_ST_FAULT / NC_ST_TIMEOUT of the first piece that failed, else 0 */
    uint32_t timeout_us;        /* per piece, 0 = 1 s */
    uint64_t t0;
    unsigned char params[MAX_PARAMS];
};

static SRWLOCK pool_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE pool_cv = CONDITION_VARIABLE_INIT;
static struct job jobs[1 + MAX_ASYNC];
static volatile LONG pool_work;     /* changes whenever a job is published */
static volatile LONG pool_alive;    /* workers that have not ended */
static volatile LONG pool_quit;
static int pool_workers;            /* started */

static int has_work(const struct job *j)
{
    return j->used && !j->cancel && j->next < (LONGLONG)j->to;
}

/* under pool_lock */
static struct job *take_job(void)
{
    int i;

    for (i = 0; i <= MAX_ASYNC; i++)
        if (has_work(&jobs[i]))
        {
            jobs[i].active++;
            return &jobs[i];
        }
    return NULL;
}

/* under pool_lock: a thread leaves the job */
static void leave_job(struct job *j)
{
    if (--j->active == 0 && j->async && (j->cancel || j->next >= (LONGLONG)j->to))
        j->done = 1;
}

/* Pieces until none are left, the job is cancelled or deadline (host us, 0: none) has passed */
static void run_pieces(struct job *j, uint64_t deadline)
{
    unsigned char copy[MAX_PARAMS];
    uint32_t *range = (uint32_t *)copy;
    uint64_t from, to;
    LONG sum = 0;

    memcpy(copy, j->params, j->size);
    while (!j->cancel)
    {
        from = (uint64_t)InterlockedExchangeAdd64(&j->next, (LONGLONG)j->chunk);
        if (from >= j->to)
            break;
        to = j->to - from < j->chunk ? j->to : from + j->chunk;
        range[0] = be32((uint32_t)from);
        range[1] = be32((uint32_t)to);
        {
            int code;

            sum += (LONG)guarded(j->kernel, copy, j->timeout_us, &code);
            if (code)
            {
                /* the job failed: no more pieces */
                InterlockedCompareExchange(&j->status, code, 0);
                j->cancel = 1;
                break;
            }
        }
        if (deadline && now_us() >= deadline)
            break;
    }
    InterlockedExchangeAdd(&j->sum, sum);
}

static DWORD WINAPI worker(LPVOID arg)
{
    HMODULE self = NULL;
    struct job *j;
    LONG seen;
    uint64_t t0;

    tls_index = (int)(INT_PTR)arg;     /* 1.. */
    /* a reference of its own: the DLL stays while this thread runs its code */
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCSTR)(void *)worker, &self);
    for (;;)
    {
        AcquireSRWLockExclusive(&pool_lock);
        while (!pool_quit && !(j = take_job()))
        {
            /* nothing to do: spin a little outside the lock, then sleep */
            seen = pool_work;
            ReleaseSRWLockExclusive(&pool_lock);
            t0 = now_us();
            while (pool_work == seen && !pool_quit && now_us() - t0 < SPIN_US)
                YieldProcessor();
            AcquireSRWLockExclusive(&pool_lock);
            if (pool_work == seen && !pool_quit && !(j = take_job()))
                SleepConditionVariableSRW(&pool_cv, &pool_lock, INFINITE, 0);
            else if (j)
                break;
        }
        if (pool_quit)
        {
            ReleaseSRWLockExclusive(&pool_lock);
            break;
        }
        ReleaseSRWLockExclusive(&pool_lock);
        run_pieces(j, 0);
        AcquireSRWLockExclusive(&pool_lock);
        leave_job(j);
        ReleaseSRWLockExclusive(&pool_lock);
    }
    InterlockedDecrement(&pool_alive);
    if (self)
        FreeLibraryAndExitThread(self, 0);
    return 0;
}

/* Starts the workers once; FALSE if none could start */
static int pool_start(void)
{
    int i, n = cores() - 1;

    if (pool_workers)
        return 1;
    for (i = 0; i < n; i++)
    {
        HANDLE h;

        InterlockedIncrement(&pool_alive);
        h = CreateThread(NULL, 0, worker, (LPVOID)(INT_PTR)(i + 1), 0, NULL);
        if (!h)
        {
            InterlockedDecrement(&pool_alive);
            break;
        }
        CloseHandle(h);
        pool_workers++;
    }
    return pool_workers > 0;
}

/* Ends the workers. Jobs still running are cancelled first (the library does that itself). */
EXPORT int32_t __cdecl ncx_shutdown(struct uni *u)
{
    int i;

    (void)u;
    /* the watchdog holds the DLL as the workers do */
    if (watchdog)
    {
        watchdog_quit = 1;
        WaitForSingleObject(watchdog, 2000);
        CloseHandle(watchdog);
        watchdog = NULL;
        watchdog_quit = 0;
        guards_started = 0;
    }
    if (!pool_workers)
        return 0;
    AcquireSRWLockExclusive(&pool_lock);
    for (i = 0; i <= MAX_ASYNC; i++)
        jobs[i].cancel = 1;
    pool_quit = 1;
    pool_work++;
    ReleaseSRWLockExclusive(&pool_lock);
    WakeAllConditionVariable(&pool_cv);
    while (pool_alive)
        SwitchToThread();
    AcquireSRWLockExclusive(&pool_lock);
    for (i = 0; i <= MAX_ASYNC; i++)
        jobs[i].used = 0;
    pool_quit = 0;
    pool_workers = 0;
    ReleaseSRWLockExclusive(&pool_lock);
    return 1;
}

/* Fills job j from the registers of ncx_run / ncx_start; FALSE if they are invalid */
static int setup_job(struct job *j, struct uni *u, int async, uint32_t timeout_us)
{
    uint32_t from = (uint32_t)u->d3, to = (uint32_t)u->d4, chunk = (uint32_t)u->d5;
    uint32_t size = (uint32_t)u->d6, params = (uint32_t)u->a1;
    const struct NCX64Module *d;
    char *base;

    if (size < 8 || size > MAX_PARAMS || from > to || !(base = membase(params, size))
        || !(d = kernel_of((uint32_t)u->d1, (uint32_t)u->d2, base)))
        return 0;
    j->async = async;
    j->kernel = d->entries[(uint32_t)u->d2];
    memcpy(j->params, base + params, size);
    j->size = size;
    j->to = to;
    j->chunk = chunk ? chunk : 1;
    j->next = (LONGLONG)from;
    j->sum = 0;
    j->active = 0;
    j->cancel = 0;
    j->done = from == to;
    j->status = 0;
    j->timeout_us = timeout_us;
    j->t0 = now_us();
    return 1;
}

static void publish(struct job *j)
{
    AcquireSRWLockExclusive(&pool_lock);
    j->used = 1;
    pool_work++;
    ReleaseSRWLockExclusive(&pool_lock);
    WakeAllConditionVariable(&pool_cv);
}

EXPORT int32_t __cdecl ncx_run(struct uni *u)
{
    struct job *j = &jobs[0];
    uint32_t threads = (uint32_t)u->d7;
    int helpers;

    if (!setup_job(j, u, 0, (uint32_t)u->a3))
    {
        put_out((uint32_t)u->a2, NC_ST_BADENTRY, 0, 0);
        return -1;
    }
    note_call("ncx_run", (uint32_t)u->d1, (uint32_t)u->d2, (uint32_t)u->a1, (uint32_t)u->d6, (uint32_t)u->d3, (uint32_t)u->d4);
    low_snap();
    helpers = threads != 1 && pool_start();
    if (helpers)
    {
        AcquireSRWLockExclusive(&pool_lock);
        j->active = 1;              /* the caller */
        ReleaseSRWLockExclusive(&pool_lock);
        publish(j);
    }
    run_pieces(j, 0);
    if (helpers)
    {
        AcquireSRWLockExclusive(&pool_lock);
        j->active--;
        ReleaseSRWLockExclusive(&pool_lock);
        while (j->active)
            YieldProcessor();
        AcquireSRWLockExclusive(&pool_lock);
        j->used = 0;
        ReleaseSRWLockExclusive(&pool_lock);
    }
    low_check("ncx_run", (uint32_t)u->d1, (uint32_t)u->d2, (uint32_t)u->a1, j->size);
    put_out((uint32_t)u->a2, j->status ? (uint32_t)j->status : NC_ST_OK, now_us() - j->t0,
            helpers ? (uint32_t)pool_workers + 1 : 1);
    return (int32_t)j->sum;
}

/*
    Asynchronous jobs (nc_start_mp, library 1.8):

      ncx_start  (d1 = handle, d2 = kernel, d3 = from, d4 = to, d5 = chunk, d6 = params size,
                  a1 = params)                       -> job 1..MAX_ASYNC, 0 if none is free
      ncx_poll   (d1 = job)                          -> 1 when nothing runs any more
      ncx_wait   (d1 = job, d2 = microseconds, a2 = out)
                 -> helps with pieces for at most that long, then: out[0] NC_ST_RUNNING, or the
                    job is over: the sum of its results, out[0] NC_ST_OK / NC_ST_CANCELLED,
                    out[1] its host microseconds, and the slot is free again
      ncx_cancel (d1 = job)                          -> pieces not started are skipped
*/
EXPORT int32_t __cdecl ncx_start(struct uni *u)
{
    struct job *j = NULL;
    int i;

    if (!pool_start())
        return 0;
    /* calls come from the emulation thread one at a time: a free slot stays free until publish */
    AcquireSRWLockExclusive(&pool_lock);
    for (i = 1; i <= MAX_ASYNC && !j; i++)
        if (!jobs[i].used && !jobs[i].active)
            j = &jobs[i];
    ReleaseSRWLockExclusive(&pool_lock);
    if (!j || !setup_job(j, u, 1, (uint32_t)u->a2))
        return 0;
    i = (int)(j - jobs);
    publish(j);
    return i;
}

static int async_jobs_running(void)
{
    int i, n = 0;

    for (i = 1; i <= MAX_ASYNC; i++)
        n += jobs[i].used && !jobs[i].done;
    return n;
}

static struct job *async_job(uint32_t n)
{
    return n >= 1 && n <= MAX_ASYNC && jobs[n].used && jobs[n].async ? &jobs[n] : NULL;
}

EXPORT int32_t __cdecl ncx_poll(struct uni *u)
{
    struct job *j = async_job((uint32_t)u->d1);
    return !j || j->done;
}

EXPORT int32_t __cdecl ncx_cancel(struct uni *u)
{
    struct job *j = async_job((uint32_t)u->d1);

    if (!j)
        return 0;
    /* every piece has started already: nothing to skip, the job ends as it is (NC_ST_OK and its
       whole result, as the hypervisor answers) */
    AcquireSRWLockExclusive(&pool_lock);
    if (!j->done && j->next < (LONGLONG)j->to)
    {
        j->cancel = 1;
        if (j->active == 0)
            j->done = 1;
    }
    ReleaseSRWLockExclusive(&pool_lock);
    return 1;
}

EXPORT int32_t __cdecl ncx_wait(struct uni *u)
{
    struct job *j = async_job((uint32_t)u->d1);
    uint32_t us = (uint32_t)u->d2;
    int32_t result;

    if (!j)
    {
        put_out((uint32_t)u->a2, NC_ST_BADJOB, 0, 0);
        return -1;
    }
    if (!j->done)
    {
        /* help: the emulation thread works on the job too, for at most us */
        struct job *h = NULL;

        AcquireSRWLockExclusive(&pool_lock);
        if (has_work(j))
        {
            j->active++;
            h = j;
        }
        ReleaseSRWLockExclusive(&pool_lock);
        if (h)
        {
            run_pieces(j, now_us() + (us ? us : 1));
            AcquireSRWLockExclusive(&pool_lock);
            leave_job(j);
            ReleaseSRWLockExclusive(&pool_lock);
        }
        else if (us)
        {
            /* nothing left to take: the last pieces run elsewhere, wait for them a little */
            uint64_t end = now_us() + us;
            while (!j->done && now_us() < end)
                YieldProcessor();
        }
    }
    if (!j->done)
    {
        put_out((uint32_t)u->a2, NC_ST_RUNNING, now_us() - j->t0, 0);
        return 0;
    }
    result = (int32_t)j->sum;
    put_out((uint32_t)u->a2, j->status ? (uint32_t)j->status : j->cancel ? NC_ST_CANCELLED : NC_ST_OK, now_us() - j->t0, (uint32_t)pool_workers);
    AcquireSRWLockExclusive(&pool_lock);
    j->used = 0;
    ReleaseSRWLockExclusive(&pool_lock);
    return result;
}
