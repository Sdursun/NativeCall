#include "hello_kernels.h"
#include <nativecall/nc_heap.h>
#include <string.h>

#if NC_NATIVE || NC_X64
#include <stdlib.h>     /* sdk/native/include: malloc() on a nc_heap */
#endif

NC_KERNEL(add)
{
    struct AddParams *p = params;

    return p->a + p->b;
}

NC_KERNEL(fault)
{
#if NC_NATIVE || NC_X64
    uintptr_t addr = 0x4;

    (void)params;
    __asm__("" : "+r"(addr));       /* hide the constant from -Warray-bounds */
    return *(volatile uint32_t *)addr;
#else
    return params == 0;             /* 0: nothing to crash on the 68k */
#endif
}

NC_KERNEL(copy)
{
    struct CopyParams *p = params;
    NC_U32 *dst = NC_PTR(NC_U32, p->dst), i, sum = 0;

    memcpy(dst, NC_PTR(NC_U32, p->src), p->count * sizeof(NC_U32));
    for (i = 0; i < p->count; i++)
        sum += NC_GET32(&dst[i]);
    return sum;
}

NC_KERNEL(mark)
{
    struct MarkParams *p = params;
    NC_U32 *dst = NC_PTR(NC_U32, p->dst), i, core = 0;
    volatile NC_U32 w;

#if NC_NATIVE
    uint64_t mpidr;
    __asm__ volatile("mrs %0, MPIDR_EL1" : "=r"(mpidr));
    core = mpidr & 3;
#endif
    for (i = p->range.from; i < p->range.to; i++)
    {
        for (w = 0; w < p->work; w++)
            ;
        NC_PUT32(&dst[i], core + 1);
    }
    return p->range.to - p->range.from;
}

NC_KERNEL(heap)
{
    struct HeapParams *p = params;
    struct NCHeap *h = nc_heap_init(NC_PTR(void, p->heap), p->size);
    void *slot[64];
    NC_U32 len[64], seed = p->seed, errors = 0, r, i, j;

    if (!h)
        return 0xEE0000FF;
    for (i = 0; i < 64; i++)
        slot[i] = 0;
    for (r = 0; r < p->rounds; r++)
    {
        seed = seed * 1103515245 + 12345;
        i = (seed >> 16) & 63;
        if (slot[i])
        {
            for (j = 0; j < len[i]; j++)
                if (((NC_U8 *)slot[i])[j] != (NC_U8)(i + j))
                    errors++;
            if (seed & 0x100)
            {
                /* grow it: realloc keeps the contents */
                NC_U32 n = len[i] + ((seed >> 4) & 255) + 1;
                void *q = nc_heap_realloc(h, slot[i], n);
                if (q)
                {
                    for (j = len[i]; j < n; j++)
                        ((NC_U8 *)q)[j] = (NC_U8)(i + j);
                    slot[i] = q;
                    len[i] = n;
                }
                continue;
            }
            nc_heap_free(h, slot[i]);
            slot[i] = 0;
        }
        else
        {
            len[i] = ((seed >> 8) & 2047) + 1;
            slot[i] = nc_heap_alloc(h, len[i]);
            if (slot[i])
                for (j = 0; j < len[i]; j++)
                    ((NC_U8 *)slot[i])[j] = (NC_U8)(i + j);
        }
    }
    for (i = 0; i < 64; i++)
        nc_heap_free(h, slot[i]);

#if NC_NATIVE || NC_X64
    {
        /* malloc() of sdk/native/include/stdlib.h on the same heap */
        char *a, *b;

        nc_heap_select(h);
        a = malloc(100);
        b = calloc(10, 10);
        if (!a || !b || b[99] != 0)
            errors++;
        else
        {
            strcpy(a, "nativecall");
            if (strlen(a) != 10 || strcmp(a, "nativecall") != 0)
                errors++;
        }
        free(a);
        free(b);
        nc_heap_select(0);
    }
#endif

    if (errors)
        return 0xEE000000 + errors;
    return nc_heap_avail(h);
}

NC_KERNEL(spin)
{
    (void)params;
#if NC_NATIVE || NC_X64
    for (;;)
        __asm__ volatile("" ::: "memory");
#else
    return 0;
#endif
}

NC_KERNEL(logtest)
{
    NC_U32 core = 0;

    (void)params;
#if NC_NATIVE
    uint64_t mpidr;
    __asm__ volatile("mrs %0, MPIDR_EL1" : "=r"(mpidr));
    core = mpidr & 3;
#endif
    nc_printf("logtest: core %u, %s, %d, 0x%08x, [%5u] [%-4d] %c%%\n", (unsigned)core, "text", -42,
              0xC0FFEEu, 7u, 3, 'z');
    (void)core;
    return 1;
}

#if NC_NATIVE || NC_X64
/* Not inlined, and each one does something after its call: no tail calls, real frames */
static __attribute__((noinline)) NC_U32 df3(volatile NC_U32 *p)
{
    return *p;
}

static __attribute__((noinline)) NC_U32 df2(volatile NC_U32 *p)
{
    NC_U32 r = df3(p);
    __asm__ volatile("" ::: "memory");
    return r + 2;
}

static __attribute__((noinline)) NC_U32 df1(volatile NC_U32 *p)
{
    NC_U32 r = df2(p);
    __asm__ volatile("" ::: "memory");
    return r + 1;
}
#endif

NC_KERNEL(deepfault)
{
#if NC_NATIVE || NC_X64
    uintptr_t addr = 0x8;

    (void)params;
    __asm__("" : "+r"(addr));
    return df1((volatile NC_U32 *)addr) + 1;
#else
    return params == 0;
#endif
}

#if NC_NATIVE
#include <nativecall/nc_abi.h>

/* 1 KB of locals per level and work after the call: no tail call, the stack really grows.
   Endless on purpose: the guard page stops it. */
#pragma GCC diagnostic ignored "-Winfinite-recursion"
static __attribute__((noinline)) NC_U32 deep(volatile NC_U32 n)
{
    volatile NC_U32 pad[256];

    pad[0] = n;
    pad[255] = deep(n + 1);
    return pad[0] + pad[255];
}
#endif

NC_KERNEL(protect)
{
#if NC_NATIVE
    struct AddParams *p = params;
    const struct NCEnv *env;
    uintptr_t cpu;

    __asm__ volatile("mrs %0, TPIDR_EL2" : "=r"(cpu));
    env = cpu >= 0x01000000UL ? *(const struct NCEnv * const *)(cpu + NC_CPU_ENV) : 0;
    if ((uintptr_t)env < 0x01000000UL || env->magic != NC_ENV_MAGIC || env->version < 7)
        return 0xEEEE;
    switch (p->a)
    {
        case 0:
        {
            volatile NC_U32 *code = (volatile NC_U32 *)(uintptr_t)env->log_write;
            *code = *code;
            break;
        }
        case 1:
        {
            uintptr_t below = (cpu & ~(uintptr_t)0x1fffff) - 0x100000;
            return *(volatile NC_U32 *)below;
        }
        case 2:
            return deep(0);
    }
    return 0;
#else
    (void)params;
    return 0xEEEE;
#endif
}

NC_MODULE(hello, HELLO_KERNELS)
