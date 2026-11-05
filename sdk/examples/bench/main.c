/*
    bench - 68k vs native vs native+NEON, Markdown table for RESULTS.md.

        bench [SIZE=<KB>] [CHUNK=<KB>] [VERTS=<n>] [REPS=<n>]

    Adler32 and CRC32 over SIZE KB (default 4096), 4x4 matrix x VERTS vertices (default 10000).
    The 68k rows run the same kernels compiled for the 68k (nc_call_68k). Native checksums go in
    CHUNK KB pieces (default 128) so that one call stays short; the longest call is reported
    (68k side measurement: includes tasks that ran right after a call). Best of REPS (5).
*/

#include <exec/types.h>
#include <exec/memory.h>
#include <devices/timer.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/timer.h>
#include <stdio.h>

#include <nativecall/nativecall.h>
#include "bench_kernels.h"

#if defined(__VBCC__)
#define COMPILER "VBCC"
#else
#define COMPILER "GCC " __VERSION__
#endif

struct Device *TimerBase;
static struct timerequest tr;

static double now_us(void)
{
    struct EClockVal ev;
    ULONG freq = ReadEClock(&ev);
    return ((double)ev.ev_hi * 4294967296.0 + (double)ev.ev_lo) * 1000000.0 / freq;
}

struct Result
{
    double best_us;
    double longest_us;
    ULONG calls;
    ULONG value;
    ULONG status;
};

/* One checksum kernel over the buffer; native in chunks, 68k in one call */
static void run_sum(struct NCKernels *k, ULONG index, BOOL native, struct SumParams *p, UBYTE *buf,
                    ULONG len, ULONG init, ULONG chunk, int reps, struct Result *r)
{
    ULONG off, n, v = init;
    double t, c;
    int i;

    r->best_us = 1e30;
    r->longest_us = 0;
    r->status = NC_ST_OK;
    for (i = 0; i < reps; i++)
    {
        r->calls = 0;
        v = init;
        t = now_us();
        if (!native)
        {
            p->buf = (ULONG)buf;
            p->len = len;
            p->init = init;
            v = nc_call_68k(k, index, p);
        }
        else
        {
            for (off = 0; off < len; off += n)
            {
                n = len - off < chunk ? len - off : chunk;
                p->buf = (ULONG)(buf + off);
                p->len = n;
                p->init = v;
                c = now_us();
                v = nc_call(k, index, p);
                c = now_us() - c;
                if (nc_status(k) != NC_ST_OK)
                    r->status = nc_status(k);
                if (c > r->longest_us)
                    r->longest_us = c;
                r->calls++;
            }
        }
        t = now_us() - t;
        if (t < r->best_us)
            r->best_us = t;
    }
    r->value = v;
}

static void run_xform(struct NCKernels *k, ULONG index, BOOL native, struct XformParams *p, int reps,
                      struct Result *r)
{
    double t;
    int i;

    r->best_us = 1e30;
    r->longest_us = 0;
    r->calls = native ? 1 : 0;
    r->status = NC_ST_OK;
    for (i = 0; i < reps; i++)
    {
        t = now_us();
        if (native)
        {
            nc_call(k, index, p);
            if (nc_status(k) != NC_ST_OK)
                r->status = nc_status(k);
        }
        else
            nc_call_68k(k, index, p);
        t = now_us() - t;
        if (t < r->best_us)
            r->best_us = t;
        if (native && t > r->longest_us)
            r->longest_us = t;
    }
}

static void row(const char *test, const char *impl, struct Result *r, double base_us, double mbytes, const char *check)
{
    char longest[32];

    if (r->calls)
        sprintf(longest, "%.0f", r->longest_us);
    else
        sprintf(longest, "-");
    printf("| %s | %s | %.2f | %.1f MB/s | %.1fx | %lu | %s | %s |\n", test, impl, r->best_us / 1000.0,
           mbytes / (r->best_us / 1000000.0), base_us / r->best_us, (unsigned long)r->calls, longest, check);
}

static const char *sum_check(struct Result *r, ULONG expect, int *rc)
{
    static char buf[64];

    if (r->status != NC_ST_OK)
        sprintf(buf, "status 0x%lx", (unsigned long)r->status);
    else if (r->value != expect)
        sprintf(buf, "WRONG %08lx", (unsigned long)r->value);
    else
    {
        sprintf(buf, "ok %08lx", (unsigned long)r->value);
        return buf;
    }
    *rc = RETURN_ERROR;
    return buf;
}

static const char *xform_check(struct Result *r, float *ref, float *out, ULONG count, int *rc)
{
    static char buf[64];
    ULONG i;
    float d, a, worst = 0;

    if (r->status != NC_ST_OK)
    {
        sprintf(buf, "status 0x%lx", (unsigned long)r->status);
        *rc = RETURN_ERROR;
        return buf;
    }
    for (i = 0; i < 4 * count; i++)
    {
        d = out[i] - ref[i];
        a = ref[i] < 0 ? -ref[i] : ref[i];
        if (d < 0) d = -d;
        if (a > 1.0f) d /= a;
        if (d > worst) worst = d;
    }
    if (worst >= 1e-5f)
        *rc = RETURN_ERROR;
    sprintf(buf, worst < 1e-5f ? "ok (max rel diff %.1e)" : "WRONG (max rel diff %.1e)", (double)worst);
    return buf;
}

#define TEMPLATE "SIZE/N,CHUNK/N,VERTS/N,REPS/N"
enum { ARG_SIZE, ARG_CHUNK, ARG_VERTS, ARG_REPS, ARG_COUNT };

int main(void)
{
    LONG args[ARG_COUNT] = { 0 };
    struct RDArgs *rda;
    ULONG size = 4096 * 1024, chunk = 128 * 1024, verts = 10000, i, seed = 12345;
    int reps = 5, rc = RETURN_OK;
    struct NCKernels *k = NULL;
    UBYTE *buf = NULL;
    float *m = NULL, *in = NULL, *ref = NULL, *out = NULL;
    struct SumParams *sp = NULL;
    struct XformParams *xp = NULL;
    struct Result r68, r;
    BOOL native;
    double mb, mbv;

    rda = ReadArgs(TEMPLATE, args, NULL);
    if (!rda)
    {
        PrintFault(IoErr(), "bench");
        return RETURN_FAIL;
    }
    if (args[ARG_SIZE])  size = *(ULONG *)args[ARG_SIZE] * 1024;
    if (args[ARG_CHUNK]) chunk = *(ULONG *)args[ARG_CHUNK] * 1024;
    if (args[ARG_VERTS]) verts = *(ULONG *)args[ARG_VERTS];
    if (args[ARG_REPS])  reps = *(ULONG *)args[ARG_REPS];

    if (OpenDevice((STRPTR)TIMERNAME, UNIT_MICROHZ, (struct IORequest *)&tr, 0) != 0)
    {
        printf("Cannot open timer.device\n");
        FreeArgs(rda);
        return RETURN_FAIL;
    }
    TimerBase = tr.tr_node.io_Device;

    k = nc_open(&bench_kernels);
    if (!k)
    {
        rc = RETURN_FAIL;
        goto out;
    }
    native = nc_is_native(k);

    buf = nc_alloc(size);
    m = nc_alloc(16 * sizeof(float));
    in = nc_alloc(4 * verts * sizeof(float));
    out = nc_alloc(4 * verts * sizeof(float));
    ref = nc_alloc(4 * verts * sizeof(float));
    sp = nc_alloc(sizeof(*sp));
    xp = nc_alloc(sizeof(*xp));
    if (!buf || !m || !in || !out || !ref || !sp || !xp)
    {
        printf("Out of memory\n");
        rc = RETURN_FAIL;
        goto out;
    }

    for (i = 0; i < size; i++)
    {
        seed = seed * 1103515245UL + 12345;
        buf[i] = (UBYTE)(seed >> 16);
    }
    for (i = 0; i < 4 * verts; i++)
    {
        seed = seed * 1103515245UL + 12345;
        in[i] = (i & 3) == 3 ? 1.0f : (float)((LONG)(seed >> 8) % 20000) / 100.0f;
    }
    /* rotation about z by 30 degrees, scale 2, translation (5, -3, 7) */
    m[0] = 1.7320508f; m[1] = -1.0f;      m[2] = 0.0f;  m[3] = 5.0f;
    m[4] = 1.0f;       m[5] = 1.7320508f; m[6] = 0.0f;  m[7] = -3.0f;
    m[8] = 0.0f;       m[9] = 0.0f;       m[10] = 2.0f; m[11] = 7.0f;
    m[12] = 0.0f;      m[13] = 0.0f;      m[14] = 0.0f; m[15] = 1.0f;

    printf("\n68k part built with %s; %s\n", COMPILER, native ? "native calls available" : "no native calls: 68k only");
    printf("Adler32/CRC32: %lu KB, native chunk %lu KB; matrix: %lu vertices; best of %d\n\n",
           (unsigned long)(size / 1024), (unsigned long)(chunk / 1024), (unsigned long)verts, reps);
    printf("| Test | Implementation | Time (ms) | Throughput | vs 68k | Native calls | Longest call (us) | Check |\n");
    printf("|---|---|---:|---:|---:|---:|---:|---|\n");

    mb = size / 1048576.0;
    mbv = 16.0 * verts / 1048576.0;

    run_sum(k, NCK_adler32, FALSE, sp, buf, size, 1, chunk, reps, &r68);
    row("Adler32", "68k", &r68, r68.best_us, mb, "reference");
    if (native)
    {
        run_sum(k, NCK_adler32, TRUE, sp, buf, size, 1, chunk, reps, &r);
        row("Adler32", "native", &r, r68.best_us, mb, sum_check(&r, r68.value, &rc));
        run_sum(k, NCK_adler32_neon, TRUE, sp, buf, size, 1, chunk, reps, &r);
        row("Adler32", "native + NEON", &r, r68.best_us, mb, sum_check(&r, r68.value, &rc));
    }

    run_sum(k, NCK_crc32_table, FALSE, sp, buf, size, 0xffffffffUL, chunk, reps, &r68);
    row("CRC32", "68k (table)", &r68, r68.best_us, mb, "reference");
    if (native)
    {
        run_sum(k, NCK_crc32_table, TRUE, sp, buf, size, 0xffffffffUL, chunk, reps, &r);
        row("CRC32", "native (table)", &r, r68.best_us, mb, sum_check(&r, r68.value, &rc));
        run_sum(k, NCK_crc32_hw, TRUE, sp, buf, size, 0xffffffffUL, chunk, reps, &r);
        row("CRC32", "native + CRC32 insn", &r, r68.best_us, mb, sum_check(&r, r68.value, &rc));
    }

    xp->matrix = (ULONG)m;
    xp->in = (ULONG)in;
    xp->count = verts;
    xp->out = (ULONG)ref;
    run_xform(k, NCK_xform, FALSE, xp, reps, &r68);
    row("Matrix 4x4", "68k FPU", &r68, r68.best_us, mbv, "reference");
    if (native)
    {
        xp->out = (ULONG)out;
        run_xform(k, NCK_xform, TRUE, xp, reps, &r);
        row("Matrix 4x4", "native", &r, r68.best_us, mbv, xform_check(&r, ref, out, verts, &rc));
        run_xform(k, NCK_xform_neon, TRUE, xp, reps, &r);
        row("Matrix 4x4", "native + NEON", &r, r68.best_us, mbv, xform_check(&r, ref, out, verts, &rc));
    }
    printf("\n");

out:
    nc_free(xp, sizeof(*xp));
    nc_free(sp, sizeof(*sp));
    nc_free(ref, 4 * verts * sizeof(float));
    nc_free(out, 4 * verts * sizeof(float));
    nc_free(in, 4 * verts * sizeof(float));
    nc_free(m, 16 * sizeof(float));
    nc_free(buf, size);
    nc_close(k);
    CloseDevice((struct IORequest *)&tr);
    FreeArgs(rda);
    return rc;
}
