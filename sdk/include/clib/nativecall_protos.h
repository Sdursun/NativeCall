/* Automatically generated header (sfdc 1.13)! Do not edit! */

#ifndef CLIB_NATIVECALL_PROTOS_H
#define CLIB_NATIVECALL_PROTOS_H

/*
**   $VER: nativecall_protos.h 1.0 $Id: nativecall_lib.sfd 1.0 (29.10.2026) $
**
**   C prototypes. For use with 32 bit integers only.
**
**   Copyright (c) 2001 Amiga, Inc.
**       All Rights Reserved
*/

#include <exec/types.h>
#include <nativecall/nc_abi.h>

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */


/* nativecall.library - native AArch64 calls for 68k programs on Emu68 */
/* (nativecall armstub on the Pi's SD card). See sdk/README.md. */
/* TRUE if native calls work on this machine */
BOOL NC_Available(void);

/* Load a .narm module (Fast RAM above 16 MB, 4 KB aligned, I-cache synced) */
APTR NC_LoadModule(CONST_STRPTR name);
VOID NC_UnloadModule(APTR module);

/* Address of entry 'index' of a module, NULL if out of range */
APTR NC_GetEntry(APTR module, ULONG index);

/* Run an entry once; 0xFFFFFFFF on error */
ULONG NC_Call(APTR entry, APTR params);

/* Same, *status receives NC_ST_* (nc_abi.h); status may be NULL */
ULONG NC_CallStatus(APTR entry, APTR params, ULONG * status);

/* Run an entry over [from, to) in pieces of about 0.5 ms; params starts with */
/* struct NCRange { ULONG from, to; }. Returns the sum of the results. */
ULONG NC_Run(APTR entry, APTR params, ULONG from, ULONG to, ULONG * status);

/* Memory native code can see (Fast RAM above 16 MB), cleared */
APTR NC_AllocMem(ULONG size);
VOID NC_FreeMem(APTR memory, ULONG size);

/* Hypervisor statistics, 8 ULONGs (NC_INFO_* in nc_abi.h) */
BOOL NC_GetInfo(ULONG * info);

/* Last native fault: ESR, FAR, ELR (low 32 bits), 3 ULONGs */
VOID NC_LastFault(ULONG * fault);

/* 1.2: load a module from memory (a copy embedded in the program); the image is copied */
APTR NC_LoadModuleMem(CONST_APTR image, ULONG size);

/* 1.2: native run time of one kernel entry (needs hypervisor 2+); reset clears it after reading. */
/* FALSE if there are no measurements for this entry. */
BOOL NC_GetKernelStats(APTR entry, struct NCKernelStats * stats, BOOL reset);

/* 1.4: NC_Run on all ARM cores Emu68 leaves free (hypervisor 3+; otherwise it is NC_Run). */
/* Each core gets its own copy of the first params_size bytes of params with its piece in */
/* the NCRange: the kernel must not write its params, and the pieces must be independent. */
ULONG NC_RunMP(APTR entry, APTR params, ULONG from, ULONG to, ULONG * status, ULONG params_size);

/* 1.4: ARM cores NC_RunMP uses (1 + helper cores), 0 without native calls */
ULONG NC_Cores(void);

/* 1.5: start NC_RunMP on the helper cores and return at once (hypervisor 4+). Returns a job */
/* handle, or 0 if it could not start (then use NC_RunMP). chunk 0: the learned piece size. */
/* The 68k must not touch the job's input or output until NC_WaitJob returns. */
ULONG NC_StartMP(APTR entry, APTR params, ULONG from, ULONG to, ULONG params_size, ULONG chunk);

/* 1.5: TRUE when the job is complete (no request, reads memory) */
BOOL NC_JobDone(ULONG job);

/* 1.5: wait for the job, return the sum of its results; status may be NULL */
ULONG NC_WaitJob(ULONG job, ULONG * status);

/* 1.6: time limit of one native call (one piece) of this module's kernels in microseconds, */
/* 0 = the hypervisor's default (1 s). Past it: NC_ST_TIMEOUT (hypervisor 6+). Returns the old value, */
/* or 0xFFFFFFFF if this machine has no working time limit. */
ULONG NC_SetTimeout(APTR module, ULONG microseconds);

/* 1.6: read the hypervisor's log (nc_printf of kernels, faults, time-outs; hypervisor 7+) from */
/* *position on (0 at first); returns the bytes copied and moves *position past them */
ULONG NC_ReadLog(APTR buffer, ULONG size, ULONG * position);

/* 1.6: details of the last native fault or time-out (registers, backtrace); FALSE if none */
BOOL NC_GetFaultInfo(struct NCFaultInfo * info);

/* 1.6: stop a job (hypervisor 7+): pieces not started are skipped, running ones end, then as */
/* NC_WaitJob; status NC_ST_CANCELLED. Jobs of a module are cancelled when it is unloaded, jobs */
/* of tasks that are gone when the next job starts. */
ULONG NC_CancelJob(ULONG job, ULONG * status);

/* 1.7: statistics of the hypervisor (8+) for a monitor: native time of each ARM core, SoC */
/* temperature, jobs, faults. FALSE without them. */
BOOL NC_GetStats(struct NCStats * stats);

/* 1.7: the modules loaded right now (who loaded them, calls, native time), at most max; returns */
/* how many there are in all */
ULONG NC_ListModules(struct NCModuleInfo * info, ULONG max);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* CLIB_NATIVECALL_PROTOS_H */
