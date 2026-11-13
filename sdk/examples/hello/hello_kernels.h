/* hello kernels: add(40, 2), a deliberate crash and tests of the SDK */
#ifndef HELLO_KERNELS_H
#define HELLO_KERNELS_H

#include <nativecall/kernel.h>

struct AddParams
{
    NC_U32 a;
    NC_U32 b;
};
NC_CHECK_SIZE(struct AddParams, 8)

struct CopyParams
{
    NC_U32 src;         /* NC_U32[count] */
    NC_U32 dst;
    NC_U32 count;
};
NC_CHECK_SIZE(struct CopyParams, 12)
NC_CHECK_OFFSET(struct CopyParams, count, 8)

struct MarkParams
{
    struct NCRange range;
    NC_U32 dst;         /* NC_U32[to] */
    NC_U32 work;        /* busy loop per unit, to give the helper cores something to take */
};
NC_CHECK_SIZE(struct MarkParams, 16)

/* add:   a + b
   fault: natively reads a chip RAM address the hypervisor does not map; the call must come
          back with NC_ST_FAULT instead of hanging the machine (68k version: returns 0)
   copy:  memcpy src -> dst, returns the sum of the copied words. Regression test: a libc
          call from a kernel must link without a GOT (see kernel.h)
   mark:  dst[i] = number of the ARM core that ran unit i, plus 1 (68k: 1); returns the
          number of units. Test of nc_run_mp: shows which cores did the work */
#define HELLO_KERNELS(K) K(add) K(fault) K(copy) K(mark) K(heap) K(spin) K(logtest) K(deepfault) K(protect)

/* protect: memory protection of hypervisor 7, params->a = what to try (struct AddParams):
              0 write a word of hypervisor code back unchanged (code is read-only)
              1 read 1 MB below the hypervisor: private memory of Emu68 (not mapped any more)
              2 overflow the stack (the lowest page of each stack is a guard)
            Each must end with NC_ST_FAULT. Returns 0 if it was not stopped, 0xEEEE without
            hypervisor 7 (nothing tried: 2 would really overwrite memory there). 68k: 0xEEEE. */

/* logtest:   nc_printf of a few formats into the hypervisor's log (hypervisor 7); returns 1.
   deepfault: crashes three functions deep (deepfault -> df1 -> df2 -> df3): the fault details
              must show where, and the way there (backtrace). 68k version: returns 0. */

/* spin:  natively an endless loop: the hypervisor's time limit must stop it with
          NC_ST_TIMEOUT instead of hanging the machine (68k version: returns 0 at once).
          Ignores its params, so it also runs under nc_run / nc_run_mp / nc_start_mp. */

/* heap:  random allocations in a nc_heap (nativecall/nc_heap.h) over 'size' bytes at 'heap';
          returns the free bytes at the end (all freed: one block again), or 0xEE000000 + the
          number of corrupted blocks. Same result on both sides. Natively also malloc()/free(). */
struct HeapParams
{
    NC_U32 heap;
    NC_U32 size;
    NC_U32 seed;
    NC_U32 rounds;
};
NC_CHECK_SIZE(struct HeapParams, 16)

NC_DECLARE(hello, HELLO_KERNELS)

#endif
