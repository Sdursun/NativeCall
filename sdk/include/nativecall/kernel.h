/*
    nativecall/kernel.h - kernels: functions written once, compiled twice.

    A kernels file (myprog_kernels.c) is compiled
      - for the Pi (aarch64 big-endian) into myprog.narm, and
      - for the 68k into the program itself, as the fallback when native calls are not
        available (other Amigas, WinUAE, Emu68 without the nativecall armstub).

    Kernels header (myprog_kernels.h), seen by both sides and by the 68k program:

        #include <nativecall/kernel.h>
        struct MyParams { struct NCRange range; NC_U32 buffer; NC_U32 count; };
        #define MYPROG_KERNELS(K) K(scale) K(checksum)
        NC_DECLARE(myprog, MYPROG_KERNELS)          // NCK_scale = 0, NCK_checksum = 1, ...

    Kernels file (myprog_kernels.c):

        #include "myprog_kernels.h"
        NC_KERNEL(scale)    { struct MyParams *p = params; ... return 0; }
        NC_KERNEL(checksum) { ... }
        NC_MODULE(myprog, MYPROG_KERNELS)           // entry table / fallback table

    Rules for kernel code (it may run at EL2 on the Pi, see sdk/README.md):
      - Plain C. No AmigaOS calls, no libc beyond memcpy/memset/memmove/memcmp.
      - Only memory the program got from nc_alloc() / NC_AllocMem(); no chip RAM, no hardware.
      - 68k structures are big-endian on both sides; use NC_U32/NC_S32 fields, and NC_PTR() for
        pointers stored in them.
      - Keep a call short (about 1 ms): use struct NCRange + nc_run() for big jobs.
      - For nc_run_mp() (several ARM cores at once): do not write the params, and let each
        piece write only its own part of the output.
      - #if NC_NATIVE ... #else ... #endif for code that exists on one side only (NEON).
*/

#ifndef NATIVECALL_KERNEL_H
#define NATIVECALL_KERNEL_H

#if defined(__aarch64__)
#if !defined(__AARCH64EB__)
#error "native kernels must be built big-endian (-mbig-endian)"
#endif
#include <stdint.h>
#include <stddef.h>
#define NC_NATIVE 1
typedef uint32_t NC_U32;
typedef int32_t NC_S32;
typedef uint16_t NC_U16;
typedef int16_t NC_S16;
typedef uint8_t NC_U8;
typedef uintptr_t NC_UPTR;      /* pointer-sized integer (64 bit here, 32 on the 68k) */
#define NC_PTR(type, addr68k)   ((type *)(uintptr_t)(NC_U32)(addr68k))
#else
#define NC_NATIVE 0
#include <stddef.h>
#include <string.h>     /* memcpy & co., as declared for the native side below */
typedef unsigned long NC_U32;
typedef long NC_S32;
typedef unsigned short NC_U16;
typedef short NC_S16;
typedef unsigned char NC_U8;
typedef unsigned long NC_UPTR;
#define NC_PTR(type, addr68k)   ((type *)(addr68k))
#endif

/*
    Layout checks for parameter structures, compiled on both sides: a mismatch between the
    68k compiler (VBCC, GCC with or without -malign-int) and the native compiler becomes a
    compile error instead of silent data corruption. Put them in the kernels header:

        NC_CHECK_SIZE(struct MyParams, 16)
        NC_CHECK_OFFSET(struct MyParams, count, 12)

    Safe layouts use 4-byte fields only (NC_U32, NC_S32, float, 68k pointers as NC_U32) and
    byte/word arrays whose size is a multiple of 4. No double: VBCC aligns it to 2 or 4 bytes.
*/
#define NC__CAT2(a, b)  a##b
#define NC__CAT(a, b)   NC__CAT2(a, b)
#define NC_CHECK_SIZE(type, size)     typedef char NC__CAT(nc_check_size_, __LINE__)[(sizeof(type) == (size)) ? 1 : -1];
#define NC_CHECK_OFFSET(type, field, offset)     typedef char NC__CAT(nc_check_offset_, __LINE__)[(offsetof(type, field) == (offset)) ? 1 : -1];

/* Params for nc_run()/NC_Run() start with this: the slice [from, to) of one call */
struct NCRange
{
    NC_U32 from;
    NC_U32 to;
};

#define NC__ENUM(name)      NCK_##name,
#define NC__PROTO(name)     NC_U32 name(void *params);

/* Kernel indexes (NCK_<name>) and prototypes; in the kernels header */
#define NC_DECLARE(prefix, LIST)                                                \
    enum { LIST(NC__ENUM) prefix##_KERNEL_COUNT };                              \
    LIST(NC__PROTO)                                                             \
    NC__DECLARE_SET(prefix)

#if NC_NATIVE

#define NC__DECLARE_SET(prefix)
#define NC_KERNEL(name)     __attribute__((used, aligned(16))) NC_U32 name(void *params)
#define NC__EXPORT(name)    __asm__(".pushsection .narm_entries,\"a\"\n\t.word " #name "\n\t.popsection");
/* Entry table of the .narm module, in list order */
#define NC_MODULE(prefix, LIST)     LIST(NC__EXPORT)

/* Hidden: -fvisibility=hidden only covers definitions. A default-visibility declaration makes
   -fPIE -fno-plt call through the GOT, which a module cannot have (NOTES.md 18). */
#pragma GCC visibility push(hidden)
void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);

/* Log (hypervisor 7+): text into the hypervisor's log, read on the Amiga with NCLog (or
   NC_ReadLog). One call writes at most 255 bytes; every core may write. Nothing happens on
   older hypervisors, and on the 68k side these do nothing at all.
   nc_printf knows %d %i %u %x %X %p %s %c %%, the flags - and 0, a width, and l / ll / z. */
void nc_log(const char *text);
void nc_printf(const char *format, ...) __attribute__((format(printf, 1, 2)));
#pragma GCC visibility pop

#else /* 68k */

typedef NC_U32 (*NCKernelFunc)(void *params);

struct NCKernelSet
{
    const char         *file;       /* module file name, loaded from PROGDIR: */
    const NCKernelFunc *fallback;   /* 68k versions, in list order */
    NC_U32              count;
    /* From <prefix>_image.c (NULL without it): the embedded module (size 0 when the module
       is not embedded) and the CRC32 of the module the program was built with */
    const unsigned char *image;
    const unsigned long *image_size;
    const unsigned long *build_id;
};

#define NC__DECLARE_SET(prefix)     extern const struct NCKernelSet prefix##_kernels;
#define NC_KERNEL(name)     NC_U32 name(void *params)
/* The log exists only for native code: the 68k versions of kernels say nothing */
#define nc_log(text)        ((void)0)
#define nc_printf(...)      ((void)0)
#define NC__FALLBACK(name)  name,

/*
    Fallback table and kernel set <prefix>_kernels, module file <prefix>.narm.

    The embedded module and the build ID come from <prefix>_image.c, which mk/nativecall.mk
    generates (tools/narm2c.py) and links in. A project with its own 68k build compiles that file
    too ("nc make narm" makes it next to the .narm), or defines NC_NO_IMAGE when compiling the
    kernels for the 68k: then the module is only loaded from PROGDIR:, without a version check.
*/
#ifdef NC_NO_IMAGE
#define NC__IMAGE(prefix)   0, 0, 0
#else
#define NC__IMAGE(prefix)   prefix##_narm_image, &prefix##_narm_image_size, &prefix##_narm_build_id
#endif

#define NC_MODULE(prefix, LIST)                                                 \
    extern const unsigned char prefix##_narm_image[];                           \
    extern const unsigned long prefix##_narm_image_size;                        \
    extern const unsigned long prefix##_narm_build_id;                          \
    static const NCKernelFunc prefix##_fallback[] = { LIST(NC__FALLBACK) 0 };   \
    const struct NCKernelSet prefix##_kernels =                                 \
        { #prefix ".narm", prefix##_fallback, prefix##_KERNEL_COUNT, NC__IMAGE(prefix) };

#endif

#endif
