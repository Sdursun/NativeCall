/*
    nativecall/nativecall.h - using kernels from a 68k program (VBCC or GCC).

        #include <nativecall/nativecall.h>
        #include "myprog_kernels.h"

        struct NCKernels *k = nc_open(&myprog_kernels);     // never fails for lack of native
        struct MyParams *p = nc_alloc(sizeof(*p));          // memory kernels may touch
        ...
        result = nc_call(k, NCK_checksum, p);               // native, or the 68k version
        total  = nc_run(k, NCK_scale, p, 0, count);         // in ~0.5 ms slices
        ...
        nc_free(p, sizeof(*p));
        nc_close(k);

    nc_open() opens nativecall.library and loads PROGDIR:<prefix>.narm. Without the library,
    without the hypervisor, or if the module does not load, every call runs the 68k version of
    the kernel: the program works everywhere, just slower.
*/

#ifndef NATIVECALL_NATIVECALL_H
#define NATIVECALL_NATIVECALL_H

#include <exec/types.h>
#include <nativecall/nc_abi.h>
#include <nativecall/kernel.h>

struct NCKernels;

/* Open a kernel set. NULL only when out of memory. The module comes from, in this order:
     1. ENV:NativeCall/ModuleDir/<prefix>.narm, if that variable is set (development)
     2. the copy embedded in the program (mk/nativecall.mk does that, NC_EMBED=1)
     3. PROGDIR:<prefix>.narm, only if its CRC32 matches the program's build ID
   If none loads, the kernels run as 68k code; nc_status() says why (NC_ST_MISMATCH, ...). */
struct NCKernels *nc_open(const struct NCKernelSet *set);

/* Same, with this module file (no version check) */
struct NCKernels *nc_open_path(const struct NCKernelSet *set, CONST_STRPTR path);
void nc_close(struct NCKernels *k);

/* TRUE if calls run natively */
BOOL nc_is_native(struct NCKernels *k);

/* Run kernel 'index' (NCK_<name>) once: natively if possible, else the 68k version */
NC_U32 nc_call(struct NCKernels *k, ULONG index, APTR params);

/* Run kernel 'index' over [from, to); params starts with struct NCRange. Natively in slices
   of about 0.5 ms (results added up), or the 68k version once over the whole range. */
NC_U32 nc_run(struct NCKernels *k, ULONG index, APTR params, ULONG from, ULONG to);

/* nc_run on several ARM cores (library 1.4 + hypervisor 3: core 0 and the cores Emu68 leaves
   idle, 3 on a PiStorm). params_size: size of the params struct, 8..256 bytes. Each core works
   on its own copy of it with its piece in the NCRange, so the kernel must not write its params,
   and pieces must not depend on each other (each writes only its own part of the output).
   Results are added up as in nc_run. Falls back to nc_run (one core) when not available. */
NC_U32 nc_run_mp(struct NCKernels *k, ULONG index, APTR params, ULONG params_size, ULONG from, ULONG to);

/* ARM cores nc_run_mp uses: 1 + helper cores; 0 without native calls */
ULONG nc_cores(struct NCKernels *k);

/* Asynchronous nc_run_mp (library 1.5 + hypervisor 4): the helper cores run the kernel while
   the 68k program goes on; its interrupts and multitasking do not stop.

       struct NCJob job;
       nc_start_mp(k, &job, NCK_decode, p, sizeof(*p), 0, frames);
       ...                                   // 68k work; leave p, its input and output alone
       if (nc_done(k, &job)) ...             // optional: cheap check, no wait
       r = nc_wait(k, &job);                 // result as nc_run_mp; nc_status(k) its status

   Rules as for nc_run_mp. Until nc_wait() returns, the 68k must not touch the job's params,
   input or output: the helpers read and write them meanwhile. nc_wait() is required even
   after nc_done() said TRUE (it orders the helpers' writes for the 68k).
   nc_start_mp() returns FALSE when it ran the job at once instead (no hypervisor 4, all four
   job slots busy, no native calls: then nc_run_mp or the 68k version); nc_wait() gives the
   result either way. nc_close() waits for jobs that were not waited for. */
struct NCJob
{
    ULONG handle;       /* library job, 0: ran at once */
    NC_U32 result;
    ULONG status;
};
BOOL nc_start_mp(struct NCKernels *k, struct NCJob *job, ULONG index, APTR params, ULONG params_size,
                 ULONG from, ULONG to);
BOOL nc_done(struct NCKernels *k, struct NCJob *job);
NC_U32 nc_wait(struct NCKernels *k, struct NCJob *job);

/* Stops a job started with nc_start_mp: pieces that have not started are skipped, pieces that
   run right now end (at most the time limit, nc_set_timeout), then like nc_wait. nc_status() is
   NC_ST_CANCELLED and the result the sum of the pieces that ran; the job's output is incomplete.
   Not needed before nc_close(): that cancels the jobs of its module. A program that ends
   without nc_close() leaves its jobs running until the next program starts one (library 1.6,
   hypervisor 7; otherwise this waits). */
NC_U32 nc_cancel(struct NCKernels *k, struct NCJob *job);

/* Always the 68k version (for comparisons) */
NC_U32 nc_call_68k(struct NCKernels *k, ULONG index, APTR params);
NC_U32 nc_run_68k(struct NCKernels *k, ULONG index, APTR params, ULONG from, ULONG to);

/* NC_ST_* of the last native call of this kernel set (NC_ST_OK, NC_ST_FAULT, ...), or after
   nc_open why there is no native module (NC_ST_NONE: no library/hypervisor, NC_ST_MISMATCH,
   NC_ST_NOMODULE) */
ULONG nc_status(struct NCKernels *k);

/* Time limit of one native call of these kernels (one piece of nc_run / nc_run_mp /
   nc_start_mp) in microseconds; 0 = the hypervisor's default, 1 s. A kernel that runs longer
   is stopped and the call ends with NC_ST_TIMEOUT: an endless loop no longer hangs the Amiga.
   Its output is then incomplete and a lock it held stays taken. Needs library 1.6 and
   hypervisor 6 (FALSE: not set; older hypervisors have no limit). */
BOOL nc_set_timeout(struct NCKernels *k, ULONG microseconds);

/* Native run time of kernel 'index' as the hypervisor measured it: calls, total, longest and
   last call in microseconds. The 68k cannot measure this itself (its interrupts wait while
   native code runs). FALSE without native calls, with library < 1.2 or hypervisor < 2. */
BOOL nc_stats(struct NCKernels *k, ULONG index, struct NCKernelStats *stats, BOOL reset);

/* Memory kernels can use: Fast RAM above 16 MB when native calls are possible, cleared.
   Pointers stored in parameter blocks must point into such memory. */
APTR nc_alloc(ULONG size);
void nc_free(APTR memory, ULONG size);

/* TRUE if native code can access [memory, memory + size): Fast RAM from 16 MB up on the Pi; any of
   the Amiga's RAM, chip RAM too, under WinUAE with the x64 backend (library 1.8). Memory the
   program got elsewhere (its BSS, AllocMem) is fine too when this says so; nc_alloc() always
   returns such memory when native calls are possible. The answer for the x64 backend needs the
   library open (nc_open); the regions are read again when it opens or closes. */
BOOL nc_addr_ok(CONST_APTR memory, ULONG size);

/* The memory region native code can see that holds the most memory: [*lo, *hi). Pointers
   inside it are fine for kernels, so a hot loop can check with two compares instead of
   calling nc_addr_ok(). FALSE (and 0, 0) if there is none. nc_addr_ok() itself also only
   compares (the regions are read once). */
BOOL nc_addr_limits(ULONG *lo, ULONG *hi);

/* A cheap clock in microseconds (wraps after ~71 minutes: use differences). On Emu68 it reads
   the ARM counter through Emu68's MOVEC registers (well under a microsecond, no Amiga bus
   access); elsewhere timer.device's EClock. timer.device costs ~20 us per call on a PiStorm,
   more than most things worth measuring. nc_now_ticks() gives the raw 64-bit counter and its
   frequency (Emu68: 54 MHz on a Pi 4). Neither needs nc_open().
   Under WinUAE with the x64 backend (library 1.8) they read the PC's clock once nc_open() found
   it (1 MHz): the emulation stands still while a kernel runs, and the Amiga's own clocks miss
   most of that time. The clock is chosen again when the library is opened or closed, so take
   both ends of a measurement while the kernels are open. */
ULONG nc_now_us(void);
void nc_now_ticks(ULONG *hi, ULONG *lo, ULONG *freq);

#endif
