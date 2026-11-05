/* proto/nativecall.h - nativecall.library for VBCC and GCC (m68k-amigaos) */
#ifndef PROTO_NATIVECALL_H
#define PROTO_NATIVECALL_H

#include <clib/nativecall_protos.h>

#ifndef __NOLIBBASE__
extern struct Library *NativeCallBase;
#endif

#if defined(__VBCC__)
#include <inline/nativecall_protos.h>
#elif defined(__GNUC__) && !defined(_NO_INLINE)
#include <inline/nativecall.h>
#endif

#endif
