/*
    nc_abi.h - interface between the 68k nativecall library and the EL2 hypervisor
    (the armstub that stays resident while Emu68 runs at EL1).

    Shared by:  hypervisor/  (aarch64, big-endian, gcc)
                amiga/       (m68k, VBCC)
                native/sdk/  (aarch64, big-endian, gcc) - module header only

    Everything here is big-endian (Emu68 runs AArch64 big-endian, see NOTES.md 1).
*/

#ifndef NC_ABI_H
#define NC_ABI_H

#if defined(__aarch64__)
#include <stdint.h>
typedef uint32_t nc_u32;
#else
typedef unsigned long nc_u32;   /* m68k: 32 bit */
#endif

#define NC_FOURCC(a,b,c,d) (((nc_u32)(a) << 24) | ((nc_u32)(b) << 16) | ((nc_u32)(c) << 8) | (nc_u32)(d))

/* ---- Call block -------------------------------------------------------------------------------

   64 bytes, 64-byte aligned, in Fast RAM. The 68k fills it in, loads the doorbell values into
   D0/D1 and executes CPUSHL DC,(A0) on it (in supervisor mode). Emu68 translates that into
   "dc civac", HCR_EL2.TPC traps it to the hypervisor, which checks D0/D1 (Emu68 keeps them in
   w19/w20) and then the block's magic, and processes the block.

   The doorbell registers are essential: every other DC CIVAC (DMA buffers, files being copied,
   Emu68's own flushes) must never be taken for a request just because the data at a 64-byte
   boundary happens to read 'NCAL' - the 68k binaries contain that constant. (The first version
   only checked memory and corrupted network buffers that way, see NOTES.md 14.)

   Without the hypervisor CPUSHL is a plain cache flush: status stays NC_ST_NONE.              */

#define NC_DOORBELL_D0      NC_FOURCC('N','C','A','L')
#define NC_DOORBELL_D1      (~NC_FOURCC('N','C','A','L') & 0xffffffffUL)

#define NC_BLOCK_SIZE       64
#define NC_BLOCK_MAGIC      NC_FOURCC('N','C','A','L')  /* set by the 68k before each request */
#define NC_BLOCK_DONE       NC_FOURCC('D','O','N','E')  /* written by the hypervisor, prevents replays */

struct NCBlock
{
    nc_u32 magic;       /* 0x00 NC_BLOCK_MAGIC */
    nc_u32 op;          /* 0x04 NC_OP_* */
    nc_u32 arg0;        /* 0x08 */
    nc_u32 arg1;        /* 0x0c */
    nc_u32 arg2;        /* 0x10 */
    nc_u32 result;      /* 0x14 */
    nc_u32 status;      /* 0x18 NC_ST_*, 68k clears it before the request */
    nc_u32 version;     /* 0x1c hypervisor version (every reply) */
    nc_u32 info[8];     /* 0x20 op dependent extra output */
};

/* Operations */
#define NC_OP_PING      1   /* result = NC_PING_REPLY */
#define NC_OP_SYNC      2   /* arg0 = start, arg1 = length: make ARM I-cache coherent with RAM */
#define NC_OP_CALL      3   /* arg0 = function, arg1 = params (may be 0), arg2 = module; result = w0 */
#define NC_OP_INFO      4   /* info[] = statistics, see NC_INFO_* */
#define NC_OP_RUNMP     5   /* hypervisor 3+: like CALL, over a range on several cores, below */
#define NC_OP_START     6   /* hypervisor 4+: RUNMP without waiting (asynchronous job), below */
#define NC_OP_WAIT      7   /* hypervisor 4+: result of a START job */

#define NC_PING_REPLY   NC_FOURCC('N','A','R','M')
#define NC_HV_VERSION   4   /* 2: CALL replies carry the native run time, 3: NC_OP_RUNMP,
                               4: NC_OP_START / NC_OP_WAIT */

/* Status */
#define NC_ST_NONE      0x00    /* nobody processed the block: no hypervisor */
#define NC_ST_OK        0x01
#define NC_ST_RUNNING   0x02    /* NC_OP_WAIT: the job is not complete yet */
#define NC_ST_BADOP     0x80
#define NC_ST_BADMOD    0x81    /* module not in RAM, not 16-byte aligned, wrong magic/version */
#define NC_ST_BADENTRY  0x82    /* function is not one of the module's entries or not 16-byte aligned */
#define NC_ST_BADADDR   0x83    /* params / SYNC range not in RAM */
#define NC_ST_FAULT     0x84    /* native code crashed: info[0..1] = ESR, info[2..3] = FAR, info[4..5] = ELR */
#define NC_ST_BUSY      0x85    /* NC_OP_START: all job slots are taken */
#define NC_ST_BADJOB    0x86    /* NC_OP_WAIT: no such job */

/* Set by the 68k library only (no request was made) */
#define NC_ST_NOCPU     0xf0    /* not a 68040/68060: CPUSHL not tried */
#define NC_ST_NOMEM     0xf1    /* no Fast RAM above 16 MB for the call block */
#define NC_ST_MISMATCH  0xf2    /* module file does not belong to this program (build ID,
                                   or fewer kernels than the program expects) */
#define NC_ST_NOMODULE  0xf3    /* no module: file missing or not loadable */

/* NC_OP_CALL (hypervisor 2+): info[] also carries the native run time of the call, including
   a faulting one. Hypervisor 1 leaves these alone: the library clears them before a call. */
#define NC_CALLINFO_TICKS   6   /* ARM counter ticks (CNTPCT_EL0) the function ran */
#define NC_CALLINFO_FREQ    7   /* counter frequency in Hz (54 MHz on a Pi 4), 0: not measured */

/* NC_OP_RUNMP (hypervisor 3+): run a kernel over [from, to) on core 0 and the ARM cores
   Emu68 leaves idle (cores 1 and 3 on a PiStorm; NOTES.md 22). Like NC_OP_CALL: arg0 =
   function, arg1 = params, arg2 = module. The params begin with { from, to } (struct NCRange)
   and are params_size bytes long. Every core runs the kernel on pieces of `chunk` units with
   its own copy of the params ({ from, to } set to the piece): the kernel must not write its
   params, and pieces must not depend on each other. result = sum of the pieces' results.

   Input:  info[0] = from, info[1] = to, info[2] = chunk (>= 1),
           info[3] = params_size (8 .. NC_MP_PARAMS_MAX, a multiple of 4)
   Output: as NC_OP_CALL (run time = the whole request, fault info of the first fault),
           and when it worked info[0] = cores that ran pieces (bit n = core n),
           info[1] = pieces run by the helper cores. */
#define NC_MPINFO_FROM      0
#define NC_MPINFO_TO        1
#define NC_MPINFO_CHUNK     2
#define NC_MPINFO_SIZE      3
#define NC_MPINFO_CORES     0
#define NC_MPINFO_HELPED    1
#define NC_MP_PARAMS_MAX    256

/* NC_OP_START (hypervisor 4+): a RUNMP job the helper cores run while the 68k goes on.
   Input as RUNMP, plus info[4] = 68k address of a struct NCJobStatus (Fast RAM, 4-byte
   aligned) that the hypervisor fills in when the job is complete (done written last).
   result = job number 0..NC_ASYNC_JOBS-1; NC_ST_BUSY when all slots are taken.

   NC_OP_WAIT: arg0 = job number. If the job is not complete, core 0 runs pieces of it for up to
   0.5 ms and the reply is NC_ST_RUNNING (ask again). If it is: the result as RUNMP (result,
   info[0] cores, info[1] pieces by helpers; or NC_ST_FAULT), and the slot is free again.
   Ask WAIT before reading the job's output even when NCJobStatus.done says it is complete:
   the request is the memory barrier that orders the helpers' writes for the 68k.

   Rules for the kernel as for RUNMP. In addition: the 68k must leave the job's input and
   output alone until WAIT says it is complete, and the module must stay loaded. A piece may
   take longer than 1 ms on a helper (the 68k is not stopped); pieces core 0 takes during WAIT
   should not. */
#define NC_ASYNC_JOBS       4
#define NC_JOBINFO_STATUS   4

struct NCJobStatus
{
    nc_u32 done;        /* 0 while running, 1 when complete */
    nc_u32 result;      /* sum of the pieces' results */
    nc_u32 status;      /* NC_ST_OK / NC_ST_FAULT (NC_ST_RUNNING while running) */
    nc_u32 cores;       /* bit n: core n ran a piece */
};

/* NC_OP_PING (hypervisor 3+): info[0] = helper cores ready for NC_OP_RUNMP (bit n = core n),
   info[1..3] = RUNMP pieces run by cores 1-3, info[4..6] = idle "wfe" of cores 1-3 taken */
#define NC_PINGINFO_HELPERS 0
#define NC_PINGINFO_PIECES  1
#define NC_PINGINFO_WFE     4

/* NC_GetKernelStats() (library 1.2): native run time of one kernel entry, microseconds.
   Only with hypervisor 2+ (otherwise all 0). */
struct NCKernelStats
{
    nc_u32 calls;       /* native calls measured */
    nc_u32 total_us;    /* sum (wraps after ~71 minutes) */
    nc_u32 max_us;      /* longest single call */
    nc_u32 last_us;
};

/* NC_OP_INFO: info[] contents */
#define NC_INFO_TRAPS       0   /* DC traps since boot (all, including Emu68's own) */
#define NC_INFO_CALLS       1   /* NC_OP_CALL requests */
#define NC_INFO_FAULTS      2   /* native faults caught */
#define NC_INFO_SKIPPED     3   /* DC ops whose address did not translate */
#define NC_INFO_LASTESR_HI  4   /* last native fault */
#define NC_INFO_LASTESR_LO  5
#define NC_INFO_LASTFAR_HI  6
#define NC_INFO_LASTFAR_LO  7

/* CPUSHL DC,(A0) */
#define NC_CPUSHL_DC_A0     0xF468

/* Emu68's fixed register mapping (NOTES.md 2): 68k D0/D1 live in AArch64 x19/x20 */
#define NC_EMU68_REG_D0     19
#define NC_EMU68_REG_D1     20

/* ---- Native module header (first bytes of a .narm file) -------------------------------------- */

#define NARM_MAGIC          NC_FOURCC('N','A','R','M')
#define NARM_VERSION        1
#define NARM_MAX_ENTRIES    1024

struct NativeHeader
{
    nc_u32 magic;           /* NARM_MAGIC */
    nc_u32 version;         /* NARM_VERSION */
    nc_u32 entry_count;
    nc_u32 entries[1];      /* entry_count offsets from the start of the binary */
};

#endif /* NC_ABI_H */
