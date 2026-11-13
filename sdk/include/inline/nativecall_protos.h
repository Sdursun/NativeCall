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

ULONG __NC_SetTimeout(__reg("a6") struct Library *, __reg("a0") APTR module, __reg("d0") ULONG microseconds)="\tjsr\t-138(a6)";
#define NC_SetTimeout(module, microseconds) __NC_SetTimeout(NativeCallBase, (module), (microseconds))

ULONG __NC_ReadLog(__reg("a6") struct Library *, __reg("a0") APTR buffer, __reg("d0") ULONG size, __reg("a1") ULONG * position)="\tjsr\t-144(a6)";
#define NC_ReadLog(buffer, size, position) __NC_ReadLog(NativeCallBase, (buffer), (size), (position))

BOOL __NC_GetFaultInfo(__reg("a6") struct Library *, __reg("a0") struct NCFaultInfo * info)="\tjsr\t-150(a6)";
#define NC_GetFaultInfo(info) __NC_GetFaultInfo(NativeCallBase, (info))

ULONG __NC_CancelJob(__reg("a6") struct Library *, __reg("d0") ULONG job, __reg("a0") ULONG * status)="\tjsr\t-156(a6)";
#define NC_CancelJob(job, status) __NC_CancelJob(NativeCallBase, (job), (status))

BOOL __NC_GetStats(__reg("a6") struct Library *, __reg("a0") struct NCStats * stats)="\tjsr\t-162(a6)";
#define NC_GetStats(stats) __NC_GetStats(NativeCallBase, (stats))

ULONG __NC_ListModules(__reg("a6") struct Library *, __reg("a0") struct NCModuleInfo * info, __reg("d0") ULONG max)="\tjsr\t-168(a6)";
#define NC_ListModules(info, max) __NC_ListModules(NativeCallBase, (info), (max))

#endif /*  _VBCCINLINE_NATIVECALL_H  */
