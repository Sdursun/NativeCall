/*
    stdlib.h for native kernels (no libc on the Pi side). Only used when building the .narm
    module; the 68k build of a kernel gets its compiler's own.

    malloc() & co. take memory from the heap chosen with nc_heap_select() (nativecall/nc_heap.h):
    a block the 68k program provides. Without a selected heap they return NULL.
    No exit(), getenv(), rand(), qsort(), printf: kernels make no system calls.
*/
#ifndef NC_NATIVE_STDLIB_H
#define NC_NATIVE_STDLIB_H

#include <stddef.h>
#include <nativecall/nc_heap.h>

static inline void *malloc(size_t n)
{
    return nc_heap_alloc(nc__heap_current, (NC_U32)n);
}

static inline void *calloc(size_t count, size_t size)
{
    size_t n = count * size, i;
    char *p;

    if (size && n / size != count)
        return 0;
    p = nc_heap_alloc(nc__heap_current, (NC_U32)n);
    if (p)
        for (i = 0; i < n; i++)
            p[i] = 0;
    return p;
}

static inline void *realloc(void *p, size_t n)
{
    return nc_heap_realloc(nc__heap_current, p, (NC_U32)n);
}

static inline void free(void *p)
{
    nc_heap_free(nc__heap_current, p);
}

static inline int abs(int x) { return x < 0 ? -x : x; }
static inline long labs(long x) { return x < 0 ? -x : x; }
static inline long long llabs(long long x) { return x < 0 ? -x : x; }

#endif
