/*
    hello - first nativecall test and test program of the SDK.

        hello                 add(40, 2) once, prints 42
        hello LOOP=1000       1000 calls, checks every result
        hello FAULT           calls a kernel that crashes natively: must report a fault
        hello MP              nc_run_mp: which ARM cores did how many units, and how fast
        hello HEAP            the nc_heap allocator on both sides: results must be equal
        hello TIME            what a call costs: clock, empty request, nc_call, nc_run_mp
        hello ASYNC           nc_start_mp: the job runs on the helper cores while the 68k counts
*/

#include <exec/types.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/nativecall.h>
#include <stdio.h>

#include <nativecall/nativecall.h>
#include "hello_kernels.h"

#define TEMPLATE "LOOP/N,FAULT/S,MP/S,HEAP/S,TIME/S,ASYNC/S"
enum { ARG_LOOP, ARG_FAULT, ARG_MP, ARG_HEAP, ARG_TIME, ARG_ASYNC, ARG_COUNT };

/* TIME: average microseconds of n runs of what, measured with nc_now_us() */
#define TIMED(n, what) \
    do { ULONG t_ = nc_now_us(), i_; for (i_ = 0; i_ < (n); i_++) { what; } \
         us_x100 = (nc_now_us() - t_) * 100 / (n); } while (0)

static void print_us(const char *name, ULONG us_x100)
{
    printf("  %-34s %3lu.%02lu us\n", name, (unsigned long)(us_x100 / 100), (unsigned long)(us_x100 % 100));
}

int main(void)
{
    LONG args[ARG_COUNT] = { 0 };
    struct RDArgs *rda;
    struct NCKernels *k;
    struct AddParams *p = NULL;
    ULONG loops = 1, i, r = 0, errors = 0;
    int rc = RETURN_OK;

    rda = ReadArgs(TEMPLATE, args, NULL);
    if (!rda)
    {
        PrintFault(IoErr(), "hello");
        return RETURN_FAIL;
    }
    if (args[ARG_LOOP])
        loops = *(ULONG *)args[ARG_LOOP];

    k = nc_open(&hello_kernels);
    if (k)
        p = nc_alloc(sizeof(*p));
    if (!p)
    {
        printf("Out of memory\n");
        rc = RETURN_FAIL;
        goto out;
    }
    if (!nc_is_native(k))
        printf("Native calls not available: 68k version\n");

    for (i = 0; i < loops; i++)
    {
        p->a = 40 + i;
        p->b = 2;
        r = nc_call(k, NCK_add, p);
        if (r != 42 + i && errors++ < 5)
            printf("Call %lu: got %lu, status 0x%lx\n", (unsigned long)i, (unsigned long)r,
                   (unsigned long)nc_status(k));
        if (SetSignal(0, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C)
            break;
    }

    if (loops == 1)
        printf("%lu (%s)\n", (unsigned long)r, nc_is_native(k) ? "native" : "68k");
    else
        printf("%lu calls (%s), %lu errors\n", (unsigned long)i, nc_is_native(k) ? "native" : "68k",
               (unsigned long)errors);
    if (errors)
        rc = RETURN_ERROR;

    /* copy: a kernel that calls memcpy (regression test for the GOT problem) */
    {
        struct CopyParams *cp = nc_alloc(sizeof(*cp));
        ULONG *a = nc_alloc(64 * sizeof(ULONG)), *b = nc_alloc(64 * sizeof(ULONG));

        if (cp && a && b)
        {
            ULONG j, expect = 0;

            for (j = 0; j < 64; j++)
            {
                a[j] = j * 3 + 1;
                expect += a[j];
            }
            cp->src = (ULONG)a;
            cp->dst = (ULONG)b;
            cp->count = 64;
            r = nc_call(k, NCK_copy, cp);
            printf("copy: %s\n", r == expect && b[63] == a[63] ? "ok" : "WRONG");
            if (r != expect || b[63] != a[63])
                rc = RETURN_ERROR;
        }
        nc_free(b, 64 * sizeof(ULONG));
        nc_free(a, 64 * sizeof(ULONG));
        nc_free(cp, sizeof(*cp));
    }

    if (args[ARG_FAULT] && nc_is_native(k))
    {
        ULONG fault[3];

        r = nc_call(k, NCK_fault, p);
        NC_LastFault(fault);
        printf("FAULT: result 0x%lx, status 0x%lx, ESR 0x%08lx, FAR 0x%08lx\n", (unsigned long)r,
               (unsigned long)nc_status(k), (unsigned long)fault[0], (unsigned long)fault[1]);
        if (nc_status(k) != NC_ST_FAULT)
            rc = RETURN_ERROR;
    }

    /* HEAP: the allocator must end in the same state on both sides */
    if (args[ARG_HEAP])
    {
        struct HeapParams *hp = nc_alloc(sizeof(*hp));
        ULONG size = 64 * 1024, r68, rn;
        APTR mem = nc_alloc(size);

        if (hp && mem)
        {
            hp->heap = (NC_U32)mem;
            hp->size = size;
            hp->seed = 4711;
            hp->rounds = 20000;
            r68 = nc_call_68k(k, NCK_heap, hp);
            rn = nc_call(k, NCK_heap, hp);
            printf("HEAP: 68k 0x%lx, %s 0x%lx, status 0x%lx: %s\n", (unsigned long)r68,
                   nc_is_native(k) ? "native" : "68k", (unsigned long)rn, (unsigned long)nc_status(k),
                   r68 == rn && r68 < 0xEE000000UL ? "ok" : "DIFFERENT");
            if (r68 != rn || r68 >= 0xEE000000UL)
                rc = RETURN_ERROR;
        }
        nc_free(mem, size);
        nc_free(hp, sizeof(*hp));
    }

    /* TIME: where the time of a call goes (68k clock; the ARM side from nc_stats) */
    if (args[ARG_TIME] && nc_is_native(k))
    {
        struct MarkParams *mp = nc_alloc(sizeof(*mp));
        ULONG *dst = nc_alloc(64 * sizeof(ULONG)), us_x100, info[8], lo, hi;
        struct NCKernelStats ks;

        if (mp && dst)
        {
            mp->dst = (NC_U32)dst;
            mp->work = 0;
            printf("Cost per call (average of 1000 to 10000):\n");
            TIMED(10000, nc_now_us());
            print_us("nc_now_us()", us_x100);
            TIMED(10000, nc_addr_ok(dst, 64));
            print_us("nc_addr_ok()", us_x100);
            TIMED(1000, NC_GetInfo(info));
            print_us("request without native code", us_x100);
            nc_stats(k, NCK_add, &ks, TRUE);
            p->a = 1;
            p->b = 2;
            TIMED(1000, nc_call(k, NCK_add, p));
            print_us("nc_call(add)", us_x100);
            if (nc_stats(k, NCK_add, &ks, FALSE) && ks.calls)
                print_us("  of which ARM time (hypervisor)", ks.total_us * 100 / ks.calls);
            TIMED(1000, nc_run(k, NCK_mark, mp, 0, 64));
            print_us("nc_run(mark, 64 units)", us_x100);
            TIMED(1000, nc_run_mp(k, NCK_mark, mp, sizeof(*mp), 0, 64));
            print_us("nc_run_mp(mark, 64 units)", us_x100);
            {
                struct NCJob job;
                ULONG t_start = 0, t;

                TIMED(1000, (t = nc_now_us(), nc_start_mp(k, &job, NCK_mark, mp, sizeof(*mp), 0, 64),
                             t_start += nc_now_us() - t, nc_wait(k, &job)));
                print_us("nc_start_mp + nc_wait (64 units)", us_x100);
                print_us("  of which nc_start_mp", t_start * 100 / 1000);
            }
            if (nc_addr_limits(&lo, &hi))
                printf("  native-visible Fast RAM: 0x%08lx-0x%08lx\n", (unsigned long)lo, (unsigned long)hi);
        }
        nc_free(dst, 64 * sizeof(ULONG));
        nc_free(mp, sizeof(*mp));
    }

    /* ASYNC: the marking job on the helper cores while the 68k keeps counting */
    if (args[ARG_ASYNC])
    {
        struct MarkParams *mp = nc_alloc(sizeof(*mp));
        ULONG units = 16384, *dst = nc_alloc(units * sizeof(ULONG));

        if (mp && dst)
        {
            ULONG count[5] = { 0 }, j, loops = 0, t0, t_start, t_done, t_wait;
            volatile ULONG work = 0;
            struct NCJob job;
            BOOL started;

            mp->dst = (NC_U32)dst;
            mp->work = 2000;
            t0 = nc_now_us();
            started = nc_start_mp(k, &job, NCK_mark, mp, sizeof(*mp), 0, units);
            t_start = nc_now_us();
            while (!nc_done(k, &job))
            {
                for (j = 0; j < 100; j++)       /* 68k work while the helpers run the job */
                    work += j;
                loops++;
            }
            t_done = nc_now_us();
            r = nc_wait(k, &job);
            t_wait = nc_now_us();

            for (j = 0; j < units; j++)
                count[dst[j] <= 4 ? dst[j] : 0]++;
            printf("ASYNC: %s, result %lu, status 0x%lx, units per core 0-3: %lu %lu %lu %lu, missing %lu\n",
                   started ? "started" : "ran at once", (unsigned long)r, (unsigned long)nc_status(k),
                   (unsigned long)count[1], (unsigned long)count[2], (unsigned long)count[3],
                   (unsigned long)count[4], (unsigned long)count[0]);
            printf("  nc_start_mp %lu us, job done after %lu us, 68k loops meanwhile %lu (sum %lu), nc_wait %lu us\n",
                   (unsigned long)(t_start - t0), (unsigned long)(t_done - t0), (unsigned long)loops,
                   (unsigned long)work, (unsigned long)(t_wait - t_done));
            if (r != units || count[0])
                rc = RETURN_ERROR;
        }
        nc_free(dst, units * sizeof(ULONG));
        nc_free(mp, sizeof(*mp));
    }

    /* MP: the same marking job on one core (nc_run) and on all (nc_run_mp) */
    if (args[ARG_MP])
    {
        struct MarkParams *mp = nc_alloc(sizeof(*mp));
        ULONG units = 16384, *dst = nc_alloc(units * sizeof(ULONG));
        struct NCKernelStats ks;
        int pass;

        if (mp && dst)
        {
            printf("ARM cores for nc_run_mp: %lu\n", (unsigned long)nc_cores(k));
            for (pass = 0; pass < 2; pass++)
            {
                ULONG count[5] = { 0 }, j;

                for (j = 0; j < units; j++)
                    dst[j] = 0;
                mp->dst = (NC_U32)dst;
                mp->work = 2000;
                nc_stats(k, NCK_mark, &ks, TRUE);
                r = pass ? nc_run_mp(k, NCK_mark, mp, sizeof(*mp), 0, units)
                         : nc_run(k, NCK_mark, mp, 0, units);
                for (j = 0; j < units; j++)
                    count[dst[j] <= 4 ? dst[j] : 0]++;
                nc_stats(k, NCK_mark, &ks, FALSE);
                printf("%-10s result %lu, status 0x%lx, units per core 0-3: %lu %lu %lu %lu, missing %lu",
                       pass ? "nc_run_mp" : "nc_run", (unsigned long)r, (unsigned long)nc_status(k),
                       (unsigned long)count[1], (unsigned long)count[2], (unsigned long)count[3],
                       (unsigned long)count[4], (unsigned long)count[0]);
                if (ks.calls)
                    printf(", %lu calls, %lu us", (unsigned long)ks.calls, (unsigned long)ks.total_us);
                printf("\n");
                if (r != units || count[0])
                    rc = RETURN_ERROR;
            }
        }
        nc_free(dst, units * sizeof(ULONG));
        nc_free(mp, sizeof(*mp));
    }

    if (nc_is_native(k))
    {
        ULONG info[8];

        if (NC_GetInfo(info))
            printf("Hypervisor: %lu traps, %lu calls, %lu faults\n", (unsigned long)info[NC_INFO_TRAPS],
                   (unsigned long)info[NC_INFO_CALLS], (unsigned long)info[NC_INFO_FAULTS]);
    }

out:
    nc_free(p, sizeof(*p));
    nc_close(k);
    FreeArgs(rda);
    return rc;
}
