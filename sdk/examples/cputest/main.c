/*
    cputest - the same CPU work on the 68k (Emu68 JIT) and as native ARM code, side by side.
    Example for the nativecall SDK: kernels in cputest_kernels.c, this file is plain 68k.

        cputest [PRIMES=<n>] [WIDTH=<w>] [HEIGHT=<h>] [ITER=<n>]
*/

#include <exec/types.h>
#include <devices/timer.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/timer.h>
#include <stdio.h>

#include <nativecall/nativecall.h>
#include "cputest_kernels.h"

#if defined(__VBCC__)
#define COMPILER "VBCC"
#elif defined(__GNUC__)
#define COMPILER "GCC " __VERSION__
#else
#define COMPILER "?"
#endif

#define BAR 40

struct Device *TimerBase;
static struct timerequest tr;

static double now_ms(void)
{
    struct EClockVal ev;
    ULONG freq = ReadEClock(&ev);
    return ((double)ev.ev_hi * 4294967296.0 + (double)ev.ev_lo) * 1000.0 / freq;
}

static void bar(const char *label, double ms, double ref_ms)
{
    int i, len = (int)(BAR * ms / ref_ms + 0.5);

    if (len < 1) len = 1;
    if (len > BAR) len = BAR;
    printf("    %-14s |", label);
    for (i = 0; i < len; i++)
        putchar('#');
    printf("\n");
}

static void line(const char *label, double ms, ULONG result, const char *unit, double ref_ms, const char *check)
{
    printf("    %-14s %9.1f ms   %10lu %s", label, ms, (unsigned long)result, unit);
    if (ref_ms > 0)
        printf("   %5.1fx faster  %s", ref_ms / ms, check);
    printf("\n");
    fflush(stdout);
}

#define TEMPLATE "PRIMES/N,WIDTH/N,HEIGHT/N,ITER/N"
enum { ARG_PRIMES, ARG_WIDTH, ARG_HEIGHT, ARG_ITER, ARG_COUNT };

int main(void)
{
    LONG args[ARG_COUNT] = { 0 };
    struct RDArgs *rda;
    struct NCKernels *k;
    struct CTParams *p;
    ULONG nprimes = 3000000, width = 640, height = 480, maxiter = 1000;
    ULONG r68, rn, rv;
    double t, ms68, msn, msv;
    int rc = RETURN_OK;

    rda = ReadArgs(TEMPLATE, args, NULL);
    if (!rda)
    {
        PrintFault(IoErr(), "cputest");
        return RETURN_FAIL;
    }
    if (args[ARG_PRIMES]) nprimes = *(ULONG *)args[ARG_PRIMES];
    if (args[ARG_WIDTH])  width = (*(ULONG *)args[ARG_WIDTH] + 3) & ~3UL;
    if (args[ARG_HEIGHT]) height = *(ULONG *)args[ARG_HEIGHT];
    if (args[ARG_ITER])   maxiter = *(ULONG *)args[ARG_ITER];

    if (OpenDevice((STRPTR)TIMERNAME, UNIT_MICROHZ, (struct IORequest *)&tr, 0) != 0)
    {
        printf("Cannot open timer.device\n");
        FreeArgs(rda);
        return RETURN_FAIL;
    }
    TimerBase = tr.tr_node.io_Device;

    k = nc_open(&cputest_kernels);
    p = k ? nc_alloc(sizeof(*p)) : NULL;
    if (!p)
    {
        printf("Out of memory\n");
        rc = RETURN_FAIL;
        goto out;
    }

    printf("\n  nativecall SDK CPU test (68k part built with %s)\n", COMPILER);
    printf("  =====================================================================\n");
    if (nc_is_native(k))
        printf("  Native ARM calls available: 68k (Emu68 JIT) vs native (Cortex-A72)\n\n");
    else
        printf("  No native calls on this machine: 68k only (the program still works)\n\n");

    /* ---- 1. Primes ---- */
    printf("  1) Prime numbers below %lu (integer: loops, branches, division)\n\n", (unsigned long)nprimes);

    t = now_ms();
    r68 = nc_run_68k(k, NCK_primes, p, 0, nprimes);
    ms68 = now_ms() - t;
    line("68k", ms68, r68, "primes", 0, "");

    if (nc_is_native(k))
    {
        t = now_ms();
        rn = nc_run(k, NCK_primes, p, 0, nprimes);
        msn = now_ms() - t;
        if (nc_status(k) != NC_ST_OK || rn != r68) rc = RETURN_ERROR;
        line("native ARM", msn, rn, "primes", ms68, rn == r68 ? "(same result)" : "(DIFFERENT!)");

        printf("\n");
        bar("68k", ms68, ms68);
        bar("native ARM", msn, ms68);
    }

    /* ---- 2. Mandelbrot ---- */
    printf("\n  2) Mandelbrot %lux%lu, max %lu iterations (floating point)\n\n",
           (unsigned long)width, (unsigned long)height, (unsigned long)maxiter);
    p->width = width;
    p->height = height;
    p->maxiter = maxiter;

    t = now_ms();
    r68 = nc_run_68k(k, NCK_mandel, p, 0, height);
    ms68 = now_ms() - t;
    line("68k FPU", ms68, r68, "iterations", 0, "");

    if (nc_is_native(k))
    {
        char check[40];
        double diff;

        t = now_ms();
        rn = nc_run(k, NCK_mandel, p, 0, height);
        msn = now_ms() - t;
        if (nc_status(k) != NC_ST_OK || rn != r68) rc = RETURN_ERROR;
        line("native ARM", msn, rn, "iterations", ms68, rn == r68 ? "(same result)" : "(DIFFERENT!)");

        t = now_ms();
        rv = nc_run(k, NCK_mandel_neon, p, 0, height);
        msv = now_ms() - t;
        diff = 100.0 * ((double)rv - (double)r68) / (double)r68;
        if (diff < 0) diff = -diff;
        /* single precision drifts away from double at the edge of the set */
        sprintf(check, diff < 2.0 ? "(float: %.2f%% off)" : "(DIFFERENT: %.2f%%)", diff);
        if (nc_status(k) != NC_ST_OK || diff >= 2.0) rc = RETURN_ERROR;
        line("native NEON", msv, rv, "iterations", ms68, check);

        printf("\n");
        bar("68k FPU", ms68, ms68);
        bar("native ARM", msn, ms68);
        bar("native NEON", msv, ms68);
    }
    printf("\n");

out:
    if (p) nc_free(p, sizeof(*p));
    nc_close(k);
    CloseDevice((struct IORequest *)&tr);
    FreeArgs(rda);
    return rc;
}
