#ifndef _VBCCINLINE_NATIVECALL_H
#define _VBCCINLINE_NATIVECALL_H

#ifndef EXEC_TYPES_H
#include <exec/types.h>
#endif

BOOL __NC_Available(__reg("a6") struct Library *)="\tjsr\t-30(a6)";
#define NC_Available() __NC_Available(NativeCallBase)

APTR __NC_LoadModule(__reg("a6") struct Library *, __reg("a0") CONST_STRPTR name)="\tjsr\t-36(a6)";
#define NC_LoadModule(name) __NC_LoadModule(NativeCallBase, (name))

VOID __NC_UnloadModule(__reg("a6") struct Library *, __reg("a0") APTR module)="\tjsr\t-42(a6)";
#define NC_UnloadModule(module) __NC_UnloadModule(NativeCallBase, (module))

APTR __NC_GetEntry(__reg("a6") struct Library *, __reg("a0") APTR module, __reg("d0") ULONG index)="\tjsr\t-48(a6)";
#define NC_GetEntry(module, index) __NC_GetEntry(NativeCallBase, (module), (index))

ULONG __NC_Call(__reg("a6") struct Library *, __reg("a0") APTR entry, __reg("a1") APTR params)="\tjsr\t-54(a6)";
#define NC_Call(entry, params) __NC_Call(NativeCallBase, (entry), (params))

ULONG __NC_CallStatus(__reg("a6") struct Library *, __reg("a0") APTR entry, __reg("a1") APTR params, __reg("a2") ULONG * status)="\tjsr\t-60(a6)";
#define NC_CallStatus(entry, params, status) __NC_CallStatus(NativeCallBase, (entry), (params), (status))

ULONG __NC_Run(__reg("a6") struct Library *, __reg("a0") APTR entry, __reg("a1") APTR params, __reg("d0") ULONG from, __reg("d1") ULONG to, __reg("a2") ULONG * status)="\tjsr\t-66(a6)";
#define NC_Run(entry, params, from, to, status) __NC_Run(NativeCallBase, (entry), (params), (from), (to), (status))

APTR __NC_AllocMem(__reg("a6") struct Library *, __reg("d0") ULONG size)="\tjsr\t-72(a6)";
#define NC_AllocMem(size) __NC_AllocMem(NativeCallBase, (size))

VOID __NC_FreeMem(__reg("a6") struct Library *, __reg("a1") APTR memory, __reg("d0") ULONG size)="\tjsr\t-78(a6)";
#define NC_FreeMem(memory, size) __NC_FreeMem(NativeCallBase, (memory), (size))

BOOL __NC_GetInfo(__reg("a6") struct Library *, __reg("a0") ULONG * info)="\tjsr\t-84(a6)";
#define NC_GetInfo(info) __NC_GetInfo(NativeCallBase, (info))

VOID __NC_LastFault(__reg("a6") struct Library *, __reg("a0") ULONG * fault)="\tjsr\t-90(a6)";
#define NC_LastFault(fault) __NC_LastFault(NativeCallBase, (fault))

APTR __NC_LoadModuleMem(__reg("a6") struct Library *, __reg("a0") void * image, __reg("d0") ULONG size)="\tjsr\t-96(a6)";
#define NC_LoadModuleMem(image, size) __NC_LoadModuleMem(NativeCallBase, (void *)(image), (size))

BOOL __NC_GetKernelStats(__reg("a6") struct Library *, __reg("a0") APTR entry, __reg("a1") struct NCKernelStats * stats, __reg("d0") LONG reset)="\tjsr\t-102(a6)";
#define NC_GetKernelStats(entry, stats, reset) __NC_GetKernelStats(NativeCallBase, (entry), (stats), (reset))

ULONG __NC_RunMP(__reg("a6") struct Library *, __reg("a0") APTR entry, __reg("a1") APTR params, __reg("d0") ULONG from, __reg("d1") ULONG to, __reg("a2") ULONG * status, __reg("d2") ULONG params_size)="\tjsr\t-108(a6)";
#define NC_RunMP(entry, params, from, to, status, params_size) __NC_RunMP(NativeCallBase, (entry), (params), (from), (to), (status), (params_size))

ULONG __NC_Cores(__reg("a6") struct Library *)="\tjsr\t-114(a6)";
#define NC_Cores() __NC_Cores(NativeCallBase)

ULONG __NC_StartMP(__reg("a6") struct Library *, __reg("a0") APTR entry, __reg("a1") APTR params, __reg("d0") ULONG from, __reg("d1") ULONG to, __reg("d2") ULONG params_size, __reg("d3") ULONG chunk)="\tjsr\t-120(a6)";
#define NC_StartMP(entry, params, from, to, params_size, chunk) __NC_StartMP(NativeCallBase, (entry), (params), (from), (to), (params_size), (chunk))

BOOL __NC_JobDone(__reg("a6") struct Library *, __reg("d0") ULONG job)="\tjsr\t-126(a6)";
#define NC_JobDone(job) __NC_JobDone(NativeCallBase, (job))

ULONG __NC_WaitJob(__reg("a6") struct Library *, __reg("d0") ULONG job, __reg("a0") ULONG * status)="\tjsr\t-132(a6)";
#define NC_WaitJob(job, status) __NC_WaitJob(NativeCallBase, (job), (status))

#endif /*  _VBCCINLINE_NATIVECALL_H  */
