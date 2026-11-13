/* Automatically generated header (sfdc 1.13)! Do not edit! */

#ifndef _INLINE_NATIVECALL_H
#define _INLINE_NATIVECALL_H

#ifndef _SFDC_VARARG_DEFINED
#define _SFDC_VARARG_DEFINED
#ifdef __HAVE_IPTR_ATTR__
typedef APTR _sfdc_vararg __attribute__((iptr));
#else
typedef ULONG _sfdc_vararg;
#endif /* __HAVE_IPTR_ATTR__ */
#endif /* _SFDC_VARARG_DEFINED */

#ifndef __INLINE_MACROS_H
#include <inline/macros.h>
#endif /* !__INLINE_MACROS_H */

#ifndef NATIVECALL_BASE_NAME
#define NATIVECALL_BASE_NAME NativeCallBase
#endif /* !NATIVECALL_BASE_NAME */

#define NC_Available() \
      LP0(0x1e, BOOL, NC_Available ,\
      , NATIVECALL_BASE_NAME)

#define NC_LoadModule(___name) \
      LP1(0x24, APTR, NC_LoadModule , CONST_STRPTR, ___name, a0,\
      , NATIVECALL_BASE_NAME)

#define NC_UnloadModule(___module) \
      LP1NR(0x2a, NC_UnloadModule , APTR, ___module, a0,\
      , NATIVECALL_BASE_NAME)

#define NC_GetEntry(___module, ___index) \
      LP2(0x30, APTR, NC_GetEntry , APTR, ___module, a0, ULONG, ___index, d0,\
      , NATIVECALL_BASE_NAME)

#define NC_Call(___entry, ___params) \
      LP2(0x36, ULONG, NC_Call , APTR, ___entry, a0, APTR, ___params, a1,\
      , NATIVECALL_BASE_NAME)

#define NC_CallStatus(___entry, ___params, ___status) \
      LP3(0x3c, ULONG, NC_CallStatus , APTR, ___entry, a0, APTR, ___params, a1, ULONG *, ___status, a2,\
      , NATIVECALL_BASE_NAME)

#define NC_Run(___entry, ___params, ___from, ___to, ___status) \
      LP5(0x42, ULONG, NC_Run , APTR, ___entry, a0, APTR, ___params, a1, ULONG, ___from, d0, ULONG, ___to, d1, ULONG *, ___status, a2,\
      , NATIVECALL_BASE_NAME)

#define NC_AllocMem(___size) \
      LP1(0x48, APTR, NC_AllocMem , ULONG, ___size, d0,\
      , NATIVECALL_BASE_NAME)

#define NC_FreeMem(___memory, ___size) \
      LP2NR(0x4e, NC_FreeMem , APTR, ___memory, a1, ULONG, ___size, d0,\
      , NATIVECALL_BASE_NAME)

#define NC_GetInfo(___info) \
      LP1(0x54, BOOL, NC_GetInfo , ULONG *, ___info, a0,\
      , NATIVECALL_BASE_NAME)

#define NC_LastFault(___fault) \
      LP1NR(0x5a, NC_LastFault , ULONG *, ___fault, a0,\
      , NATIVECALL_BASE_NAME)

#define NC_LoadModuleMem(___image, ___size) \
      LP2(0x60, APTR, NC_LoadModuleMem , CONST_APTR, ___image, a0, ULONG, ___size, d0,\
      , NATIVECALL_BASE_NAME)

#define NC_GetKernelStats(___entry, ___stats, ___reset) \
      LP3(0x66, BOOL, NC_GetKernelStats , APTR, ___entry, a0, struct NCKernelStats *, ___stats, a1, BOOL, ___reset, d0,\
      , NATIVECALL_BASE_NAME)

#define NC_RunMP(___entry, ___params, ___from, ___to, ___status, ___params_size) \
      LP6(0x6c, ULONG, NC_RunMP , APTR, ___entry, a0, APTR, ___params, a1, ULONG, ___from, d0, ULONG, ___to, d1, ULONG *, ___status, a2, ULONG, ___params_size, d2,\
      , NATIVECALL_BASE_NAME)

#define NC_Cores() \
      LP0(0x72, ULONG, NC_Cores ,\
      , NATIVECALL_BASE_NAME)

#define NC_StartMP(___entry, ___params, ___from, ___to, ___params_size, ___chunk) \
      LP6(0x78, ULONG, NC_StartMP , APTR, ___entry, a0, APTR, ___params, a1, ULONG, ___from, d0, ULONG, ___to, d1, ULONG, ___params_size, d2, ULONG, ___chunk, d3,\
      , NATIVECALL_BASE_NAME)

#define NC_JobDone(___job) \
      LP1(0x7e, BOOL, NC_JobDone , ULONG, ___job, d0,\
      , NATIVECALL_BASE_NAME)

#define NC_WaitJob(___job, ___status) \
      LP2(0x84, ULONG, NC_WaitJob , ULONG, ___job, d0, ULONG *, ___status, a0,\
      , NATIVECALL_BASE_NAME)

#define NC_SetTimeout(___module, ___microseconds) \
      LP2(0x8a, ULONG, NC_SetTimeout , APTR, ___module, a0, ULONG, ___microseconds, d0,\
      , NATIVECALL_BASE_NAME)

#define NC_ReadLog(___buffer, ___size, ___position) \
      LP3(0x90, ULONG, NC_ReadLog , APTR, ___buffer, a0, ULONG, ___size, d0, ULONG *, ___position, a1,\
      , NATIVECALL_BASE_NAME)

#define NC_GetFaultInfo(___info) \
      LP1(0x96, BOOL, NC_GetFaultInfo , struct NCFaultInfo *, ___info, a0,\
      , NATIVECALL_BASE_NAME)

#define NC_CancelJob(___job, ___status) \
      LP2(0x9c, ULONG, NC_CancelJob , ULONG, ___job, d0, ULONG *, ___status, a0,\
      , NATIVECALL_BASE_NAME)

#define NC_GetStats(___stats) \
      LP1(0xa2, BOOL, NC_GetStats , struct NCStats *, ___stats, a0,\
      , NATIVECALL_BASE_NAME)

#define NC_ListModules(___info, ___max) \
      LP2(0xa8, ULONG, NC_ListModules , struct NCModuleInfo *, ___info, a0, ULONG, ___max, d0,\
      , NATIVECALL_BASE_NAME)

#endif /* !_INLINE_NATIVECALL_H */
