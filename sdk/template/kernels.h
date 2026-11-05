/* @NAME@ kernels: seen by the 68k program and by @NAME@.narm */
#ifndef @UNAME@_KERNELS_H
#define @UNAME@_KERNELS_H

#include <nativecall/kernel.h>

/* Parameter blocks: NC_U32 fields only, NCRange first for nc_run() */
struct SumParams
{
    struct NCRange range;
    NC_U32 data;            /* NC_U32 * (68k address), from nc_alloc() */
};

/* One line per kernel; the order is the entry order of @NAME@.narm */
#define @UNAME@_KERNELS(K) \
    K(sum)

NC_DECLARE(@NAME@, @UNAME@_KERNELS)

#endif
