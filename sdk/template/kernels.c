/* @NAME@ kernels: compiled for the Pi (@NAME@.narm) and for the 68k (fallback).
   Rules: sdk/README.md "Kernel rules" - plain C, no AmigaOS, only nc_alloc() memory,
   keep a call short. sum may run on several ARM cores at once (nc_run_mp): it only reads
   its params and its own part of the data. */

#include "@NAME@_kernels.h"

NC_KERNEL(sum)
{
    struct SumParams *p = params;
    NC_U32 *data = NC_PTR(NC_U32, p->data);
    NC_U32 i, sum = 0;

    for (i = p->range.from; i < p->range.to; i++)
        sum += data[i];
    return sum;
}

NC_MODULE(@NAME@, @UNAME@_KERNELS)
