/*
    hello - first nativecall test and test program of the SDK.

        hello                 add(40, 2) once, prints 42
        hello LOOP=1000       1000 calls, checks every result
        hello FAULT           calls a kernel that crashes natively: must report a fault
        hello MP              nc_run_mp: which ARM cores did how many units, and how fast
        hello HEAP            the nc_heap allocator on both sides: results must be equal
        hello TIME            what a call costs: clock, empty request, nc_call, nc_run_mp
        hello ASYNC           nc_start_mp: the job runs on the helper cores while the 68k counts
        hello TIMEOUT         a kernel that never returns, under nc_call, nc_run_mp and nc_start_mp:
                              each must end with NC_ST_TIMEOUT (library 1.6, hypervisor 6)
        hello LOG             nc_printf from a kernel into the hypervisor's log, read back here; a
                              crash three functions deep and its details: registers, backtrace
                              (library 1.6, hypervisor 7)
        hello PROTECT         memory protection (hypervisor 7): writing hypervisor code, reading the
                              private memory of Emu68 and a stack overflow must each be a fault
        hello LEAK            (test) starts a job and ends without nc_wait / nc_close
        hello CANCEL          nc_cancel stops a running job; nc_close stops the jobs of its module
                              (library 1.6, hypervisor 7)
*/

#include <exec/types.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/nativecall.h>
#include <stdio.h>
#include <string.h>

#include <nativecall/nativecall.h>
#include "hello_kernels.h"

#define TEMPLATE "LOOP/N,FAULT/S,MP/S,HEAP/S,TIME/S,ASYNC/S,TIMEOUT/S,LOG/S,PROTECT/S,CANCEL/S,LEAK/S"
enum { ARG_LOOP, ARG_FAULT, ARG_MP, ARG_HEAP, ARG_TIME, ARG_ASYNC, ARG_TIMEOUT, ARG_LOG, ARG_PROTECT, ARG_CANCEL, ARG_LEAK, ARG_COUNT };

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

    /* TIMEOUT: spin never returns natively; 20 ms limit per call / piece */
    if (args[ARG_TIMEOUT] && nc_is_native(k))
    {
        struct MarkParams *mp = nc_alloc(sizeof(*mp));
        ULONG fault[3], t0, t1, st;
        struct NCJob job;

        if (!nc_set_timeout(k, 20000))
            printf("TIMEOUT: no time limit here (needs nativecall.library 1.6)\n");
        else if (mp)
        {
            mp->dst = 0;
            mp->work = 0;
            t0 = nc_now_us();
            r = nc_call(k, NCK_spin, mp);
            t1 = nc_now_us();
            NC_LastFault(fault);
            st = nc_status(k);
            printf("TIMEOUT nc_call:     status 0x%lx after %lu us, ELR 0x%08lx\n", (unsigned long)st,
                   (unsigned long)(t1 - t0), (unsigned long)fault[2]);
            if (st != NC_ST_TIMEOUT)
                rc = RETURN_ERROR;

            p->a = 40;
            p->b = 2;
            r = nc_call(k, NCK_add, p);
            printf("  then add(40, 2) = %lu, status 0x%lx\n", (unsigned long)r, (unsigned long)nc_status(k));
            if (r != 42 || nc_status(k) != NC_ST_OK)
                rc = RETURN_ERROR;

            t0 = nc_now_us();
            nc_run_mp(k, NCK_spin, mp, sizeof(*mp), 0, 256);
            t1 = nc_now_us();
            st = nc_status(k);
            printf("TIMEOUT nc_run_mp:   status 0x%lx after %lu us\n", (unsigned long)st, (unsigned long)(t1 - t0));
            if (st != NC_ST_TIMEOUT)
                rc = RETURN_ERROR;

            t0 = nc_now_us();
            nc_start_mp(k, &job, NCK_spin, mp, sizeof(*mp), 0, 256);
            nc_wait(k, &job);
            t1 = nc_now_us();
            st = nc_status(k);
            printf("TIMEOUT nc_start_mp: status 0x%lx after %lu us\n", (unsigned long)st, (unsigned long)(t1 - t0));
            if (st != NC_ST_TIMEOUT)
                rc = RETURN_ERROR;

            nc_set_timeout(k, 0);
            r = nc_call(k, NCK_add, p);
            printf("  then add(40, 2) = %lu, status 0x%lx\n", (unsigned long)r, (unsigned long)nc_status(k));
            if (r != 42 || nc_status(k) != NC_ST_OK)
                rc = RETURN_ERROR;
        }
        nc_free(mp, sizeof(*mp));
    }

    /* LOG: nc_printf into the hypervisor's log, then a deep crash and its details */
    if (args[ARG_LOG] && nc_is_native(k))
    {
        static char text[NC_LOG_SIZE + 1];
        struct NCFaultInfo fi;
        ULONG pos = 0, n, j, in_module = 0;

        if (NativeCallBase->lib_Version == 1 && NativeCallBase->lib_Revision < 6)
            printf("LOG: needs nativecall.library 1.6\n");
        else
        {
            /* skip what is in the log already, then one line of our own */
            while (NC_ReadLog(text, NC_LOG_SIZE, &pos))
                ;
            r = nc_call(k, NCK_logtest, p);
            n = NC_ReadLog(text, NC_LOG_SIZE, &pos);
            text[n] = 0;
            printf("LOG: logtest status 0x%lx, %lu bytes in the log:\n%s", (unsigned long)nc_status(k),
                   (unsigned long)n, text);
            if (n == 0 || !strstr(text, "logtest: core 0, text, -42, 0x00c0ffee, [    7] [3   ] z%"))
            {
                printf("LOG: WRONG (no log: hypervisor < 7?)\n");
                rc = RETURN_ERROR;
            }

            r = nc_call(k, NCK_deepfault, p);
            printf("LOG: deepfault status 0x%lx\n", (unsigned long)nc_status(k));
            n = NC_ReadLog(text, NC_LOG_SIZE, &pos);
            text[n] = 0;
            printf("  hypervisor says: %s", n ? text : "(nothing)\n");
            if (NC_GetFaultInfo(&fi))
            {
                printf("  fault %lu: status 0x%lx, core %lu, ESR 0x%08lx, FAR 0x%08lx, module 0x%08lx\n  backtrace:",
                       (unsigned long)fi.count, (unsigned long)fi.status, (unsigned long)fi.core,
                       (unsigned long)fi.esr, (unsigned long)fi.far_lo, (unsigned long)fi.module);
                for (j = 0; j < NC_FAULT_FRAMES; j++)
                    if (fi.backtrace[j] != NC_FAULT_NOOFFSET)
                    {
                        printf(" 0x%lx", (unsigned long)fi.backtrace[j]);
                        in_module++;
                    }
                printf("\n  x0 0x%08lx%08lx (the bad pointer), x30 0x%08lx%08lx\n", (unsigned long)fi.x[0][0],
                       (unsigned long)fi.x[0][1], (unsigned long)fi.x[30][0], (unsigned long)fi.x[30][1]);
            }
            /* where (df3), the link register (df2), then df1 and deepfault from the frames */
            if (nc_status(k) != NC_ST_FAULT || in_module < 4 || fi.far_lo != 8 || fi.x[0][1] != 8)
            {
                printf("LOG: fault details WRONG\n");
                rc = RETURN_ERROR;
            }
        }
    }

    /* PROTECT: three things native code must not do; each must be stopped with a fault */
    if (args[ARG_PROTECT] && nc_is_native(k))
    {
        static const char *what[3] = { "write hypervisor code", "read Emu68 memory", "stack overflow" };
        ULONG mode, fault[3];

        for (mode = 0; mode < 3; mode++)
        {
            p->a = mode;
            p->b = 0;
            r = nc_call(k, NCK_protect, p);
            NC_LastFault(fault);
            if (r == 0xEEEE)
            {
                printf("PROTECT: needs hypervisor 7, nothing tried\n");
                break;
            }
            printf("PROTECT %-21s status 0x%lx, ESR 0x%08lx, FAR 0x%08lx\n", what[mode],
                   (unsigned long)nc_status(k), (unsigned long)fault[0], (unsigned long)fault[1]);
            if (nc_status(k) != NC_ST_FAULT)
                rc = RETURN_ERROR;
        }
        p->a = 40;
        p->b = 2;
        r = nc_call(k, NCK_add, p);
        printf("  then add(40, 2) = %lu, status 0x%lx\n", (unsigned long)r, (unsigned long)nc_status(k));
        if (r != 42)
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

    /* CANCEL: a job of ~90 ms stopped after 10 ms; then one stopped by nc_close */
    if (args[ARG_CANCEL] && nc_is_native(k))
    {
        struct MarkParams *mp = nc_alloc(sizeof(*mp));
        ULONG units = 16384, *dst = nc_alloc(units * sizeof(ULONG));
        struct NCKernels *k2 = nc_open(&hello_kernels);

        if (mp && dst && k2 && nc_is_native(k2))
        {
            ULONG j, done = 0, t0, t1, t2;
            struct NCJob job;
            BOOL started;

            mp->dst = (NC_U32)dst;
            mp->work = 2000;
            t0 = nc_now_us();
            started = nc_start_mp(k, &job, NCK_mark, mp, sizeof(*mp), 0, units);
            while (nc_now_us() - t0 < 10000)
                ;
            t1 = nc_now_us();
            r = nc_cancel(k, &job);
            t2 = nc_now_us();
            for (j = 0; j < units; j++)
                done += dst[j] != 0;
            printf("CANCEL nc_cancel: %s, status 0x%lx, result %lu, %lu of %lu units done, cancel took %lu us\n",
                   started ? "started" : "ran at once", (unsigned long)nc_status(k), (unsigned long)r,
                   (unsigned long)done, (unsigned long)units, (unsigned long)(t2 - t1));
            if (started && nc_status(k) == NC_ST_OK && done == units)
                printf("  (this hypervisor cannot cancel: the job was waited for)\n");
            else if (started && (nc_status(k) != NC_ST_CANCELLED || done == 0 || done >= units || r != done))
                rc = RETURN_ERROR;

            /* the same job on a second kernel set, closed while it runs: nc_close cancels it */
            for (j = 0; j < units; j++)
                dst[j] = 0;
            t0 = nc_now_us();
            started = nc_start_mp(k2, &job, NCK_mark, mp, sizeof(*mp), 0, units);
            nc_close(k2);
            k2 = NULL;
            t1 = nc_now_us();
            for (j = 0, done = 0; j < units; j++)
                done += dst[j] != 0;
            printf("CANCEL nc_close:  %s, back after %lu us, %lu of %lu units done\n",
                   started ? "started" : "ran at once", (unsigned long)(t1 - t0), (unsigned long)done,
                   (unsigned long)units);
            /* and jobs still work */
            r = nc_run_mp(k, NCK_mark, mp, sizeof(*mp), 0, 256);
            printf("  then nc_run_mp = %lu, status 0x%lx\n", (unsigned long)r, (unsigned long)nc_status(k));
            if (r != 256)
                rc = RETURN_ERROR;
        }
        if (k2)
            nc_close(k2);
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

    /* LEAK (test of the library): what a careless program does: start a job and end without
       nc_wait or nc_close. Module, params and output stay allocated on purpose (the job writes
       them). The library must cancel the job and take its slot back when the next program
       starts one: after more than four of these, "hello ASYNC" must still say "started". */
    if (args[ARG_LEAK] && nc_is_native(k))
    {
        struct MarkParams *mp = nc_alloc(sizeof(*mp));
        ULONG units = 16384, *dst = nc_alloc(units * sizeof(ULONG));
        struct NCJob job;

        if (mp && dst)
        {
            mp->dst = (NC_U32)dst;
            mp->work = 2000;
            printf("LEAK: job %s, leaving without nc_wait and nc_close\n",
                   nc_start_mp(k, &job, NCK_mark, mp, sizeof(*mp), 0, units) ? "started" : "ran at once");
            FreeArgs(rda);
            return rc;
        }
    }

out:
    nc_free(p, sizeof(*p));
    nc_close(k);
    FreeArgs(rda);
    return rc;
}
