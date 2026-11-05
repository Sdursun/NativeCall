/*
    @NAME@ - nativecall program.

    The work is in @NAME@_kernels.c: it runs natively on Emu68 with the nativecall
    armstub, and as plain 68k code everywhere else.
*/

#include <exec/types.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <stdio.h>

#include <nativecall/nativecall.h>
#include "@NAME@_kernels.h"

#define COUNT 10000

int main(void)
{
    struct NCKernels *k;
    struct SumParams *p;
    NC_U32 *data;
    ULONG i, sum;
    int rc = RETURN_OK;

    k = nc_open(&@NAME@_kernels);               /* opens nativecall.library, loads @NAME@.narm */
    if (!k)
        return RETURN_FAIL;

    /* Everything a kernel reads or writes comes from nc_alloc() */
    p = nc_alloc(sizeof(*p));
    data = nc_alloc(COUNT * sizeof(NC_U32));
    if (!p || !data)
    {
        printf("Out of memory\n");
        rc = RETURN_FAIL;
        goto out;
    }

    for (i = 0; i < COUNT; i++)
        data[i] = i;
    p->data = (NC_U32)data;

    /* nc_run_mp: natively in slices of about 0.5 ms on all free ARM cores, results added
       up (nc_run: the same on one core); or 68k in one go */
    sum = nc_run_mp(k, NCK_sum, p, sizeof(*p), 0, COUNT);

    printf("%s: sum of 0..%lu = %lu\n", nc_is_native(k) ? "native" : "68k",
           (unsigned long)(COUNT - 1), (unsigned long)sum);
    if (nc_is_native(k))
        printf("ARM cores: %lu\n", (unsigned long)nc_cores(k));
    if (nc_is_native(k) && nc_status(k) != NC_ST_OK)
    {
        printf("native call failed, status 0x%lx\n", (unsigned long)nc_status(k));
        rc = RETURN_ERROR;
    }

out:
    nc_free(data, COUNT * sizeof(NC_U32));
    nc_free(p, sizeof(*p));
    nc_close(k);
    return rc;
}
