#include "hello_kernels.h"
#include <nativecall/nc_heap.h>
#include <string.h>

#if NC_NATIVE
#include <stdlib.h>     /* sdk/native/include: malloc() on a nc_heap */
#endif

NC_KERNEL(add)
{
    struct AddParams *p = params;

    return p->a + p->b;
}

NC_KERNEL(fault)
{
#if NC_NATIVE
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
        sum += dst[i];
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
        dst[i] = core + 1;
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

#if NC_NATIVE
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

NC_MODULE(hello, HELLO_KERNELS)
