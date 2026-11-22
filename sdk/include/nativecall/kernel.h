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
#elif defined(__x86_64__)
/* WinUAE on the PC (x64/README.md): the same kernels in a Windows DLL that nativecall.library
   loads through uaenative.library. Little-endian: the structures defined after this header keep
   the 68k's big-endian layout (GCC's scalar_storage_order below), data behind NC_PTR() is NOT
   converted. NC_NATIVE is 0 here: kernels take their plain C paths. */
#if !defined(__GNUC__)
#error "x64 kernels are built with MinGW-w64 GCC"
#endif
#include <stdint.h>
#include <stddef.h>
#include <string.h>     /* sdk/native/include, as on the Pi: there is no C library (sdk/native/narm_rt.c) */
#define NC_NATIVE 0
#define NC_X64 1
typedef uint32_t NC_U32;
typedef int32_t NC_S32;
typedef uint16_t NC_U16;
typedef int16_t NC_S16;
typedef uint8_t NC_U8;
typedef uintptr_t NC_UPTR;
/* Host address of Amiga address 0, set by the host DLL before each call */
extern char *nc_x64_membase;
#define NC_PTR(type, addr68k)   ((type *)(void *)(nc_x64_membase + (uint32_t)(addr68k)))
typedef NC_U32 (*NCKernelFunc)(void *params);
/* Exported by every x64 module as ncx_module (sdk/x64/ncx_host.c reads it) */
struct NCX64Module
{
    uint32_t magic;                 /* NCX64_MODULE_MAGIC */
    uint32_t version;
    uint32_t count;
    const NCKernelFunc *entries;
    char **membase;
    void (**log)(const char *text, uint32_t len);  /* version 2: set by the host DLL */
};
#define NCX64_MODULE_MAGIC      0x4e584d44u     /* 'NXMD' */
#define NCX64_MODULE_VERSION    2
/* The log (host DLL 2+, read on the Amiga with NCLog): the Pi's nc_log / nc_printf (narm_rt.c),
   which write through this function of the host DLL */
extern void (*nc_x64_logfn)(const char *text, uint32_t len);
void nc_log(const char *text);
void nc_printf(const char *format, ...) __attribute__((format(printf, 1, 2)));
/* From here on structures are laid out as on the 68k: big-endian scalars */
#pragma scalar_storage_order big-endian
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

#ifndef NC_X64
#define NC_X64 0
#endif

/*
    Values in 68k memory: 68k data is big-endian. On the 68k and on the Pi (big-endian too) a
    kernel reads and writes it directly; on the PC (WinUAE, little-endian) structures defined
    after this header are big-endian by themselves, but plain arrays reached through NC_PTR()
    are not. A kernel that computes with 16-bit, 32-bit or float values from such arrays reads
    and writes them with these, the same source on all three:

        NC_U32 v = NC_GET32(&src[i]);       NC_PUT32(&dst[i], v);
        NC_U16 s = NC_GET16(&pcm[i]);       NC_PUT16(&pcm[i], s);
        float f = NC_GETF(&table[i]);       NC_PUTF(&out[i], f);

    Bytes need nothing, nor do values a kernel only copies (memcpy, a lookup it passes on), nor
    arrays of structures from the kernels header (struct Vertex *v: v[i].x is right). Plain
    loads and stores where it does not matter keep the Pi's code as it was.
*/
#if NC_X64
static inline NC_U32 nc__get32(const void *p) { uint32_t v; __builtin_memcpy(&v, p, 4); return __builtin_bswap32(v); }
static inline void nc__put32(void *p, NC_U32 v) { v = __builtin_bswap32(v); __builtin_memcpy(p, &v, 4); }
static inline NC_U16 nc__get16(const void *p) { uint16_t v; __builtin_memcpy(&v, p, 2); return __builtin_bswap16(v); }
static inline void nc__put16(void *p, NC_U16 v) { v = __builtin_bswap16(v); __builtin_memcpy(p, &v, 2); }
static inline float nc__getf(const void *p) { uint32_t v = nc__get32(p); float f; __builtin_memcpy(&f, &v, 4); return f; }
static inline void nc__putf(void *p, float f) { uint32_t v; __builtin_memcpy(&v, &f, 4); nc__put32(p, v); }
#define NC_GET32(p)     nc__get32(p)
#define NC_PUT32(p, v)  nc__put32((p), (NC_U32)(v))
#define NC_GET16(p)     nc__get16(p)
#define NC_PUT16(p, v)  nc__put16((p), (NC_U16)(v))
#define NC_GETF(p)      nc__getf(p)
#define NC_PUTF(p, v)   nc__putf((p), (float)(v))
#else
#define NC_GET32(p)     (*(const NC_U32 *)(p))
#define NC_PUT32(p, v)  (*(NC_U32 *)(p) = (NC_U32)(v))
#define NC_GET16(p)     (*(const NC_U16 *)(p))
#define NC_PUT16(p, v)  (*(NC_U16 *)(p) = (NC_U16)(v))
#define NC_GETF(p)      (*(const float *)(p))
#define NC_PUTF(p, v)   (*(float *)(p) = (float)(v))
#endif

/*
    nc_to_68k16(p, n): n 16-bit values the kernel wrote with plain stores (a library that knows
    nothing of this, a decoder's output) turned into 68k byte order, in place. Nothing on the 68k
    and the Pi.

    NC_LOCAL_STRUCTS_BEGIN / NC_LOCAL_STRUCTS_END around code whose structures only the kernel
    side uses (an included library such as minimp3): on the PC they keep the PC's own layout
    instead of the 68k's, which is faster and allows pointers to their fields. Nothing elsewhere.
*/
#if NC_X64
static inline void nc_to_68k16(void *p, NC_U32 n)
{
    uint16_t *q = (uint16_t *)p;

    while (n--)
    {
        *q = __builtin_bswap16(*q);
        q++;
    }
}
#define NC_LOCAL_STRUCTS_BEGIN  _Pragma("scalar_storage_order default")
#define NC_LOCAL_STRUCTS_END    _Pragma("scalar_storage_order big-endian")
#else
#define nc_to_68k16(p, n)       ((void)0)
#define NC_LOCAL_STRUCTS_BEGIN
#define NC_LOCAL_STRUCTS_END
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

#elif NC_X64

#define NC__DECLARE_SET(prefix)
#define NC_KERNEL(name)     NC_U32 name(void *params)
#define NC__ENTRY(name)     name,
/* Entry table of the x64 module, in list order, the variable NC_PTR() uses and the log, which
   the host DLL connects (ncx_host.c). One call writes at most 255 bytes, as on the Pi. */
#define NC_MODULE(prefix, LIST)                                                 \
    char *nc_x64_membase;                                                       \
    void (*nc_x64_logfn)(const char *, uint32_t);                               \
    static const NCKernelFunc prefix##_x64_entries[] = { LIST(NC__ENTRY) 0 };   \
    __declspec(dllexport) const struct NCX64Module ncx_module =                 \
        { NCX64_MODULE_MAGIC, NCX64_MODULE_VERSION, prefix##_KERNEL_COUNT,      \
          prefix##_x64_entries, &nc_x64_membase, &nc_x64_logfn };

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
    /* SDK for library 1.8: the embedded x64 module for WinUAE (size 0 without one) */
    const unsigned char *x64_image;
    const unsigned long *x64_image_size;
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
    generates (tools/narm2c.py) and links in, with the x64 module when the project builds one
    (NC_X64 := 1). A project with its own 68k build compiles that file
    too ("nc make narm" makes it next to the .narm), or defines NC_NO_IMAGE when compiling the
    kernels for the 68k: then the module is only loaded from PROGDIR:, without a version check.
*/
#ifdef NC_NO_IMAGE
#define NC__IMAGE(prefix)   0, 0, 0, 0, 0
#else
#define NC__IMAGE(prefix)   prefix##_narm_image, &prefix##_narm_image_size, &prefix##_narm_build_id, \
                            prefix##_nx64_image, &prefix##_nx64_image_size
#endif

#define NC_MODULE(prefix, LIST)                                                 \
    extern const unsigned char prefix##_narm_image[];                           \
    extern const unsigned long prefix##_narm_image_size;                        \
    extern const unsigned long prefix##_narm_build_id;                          \
    extern const unsigned char prefix##_nx64_image[];                           \
    extern const unsigned long prefix##_nx64_image_size;                        \
    static const NCKernelFunc prefix##_fallback[] = { LIST(NC__FALLBACK) 0 };   \
    const struct NCKernelSet prefix##_kernels =                                 \
        { #prefix ".narm", prefix##_fallback, prefix##_KERNEL_COUNT, NC__IMAGE(prefix) };

#endif

#endif
