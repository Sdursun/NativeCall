/* bench kernels: checksums and a 4x4 matrix transform */
#ifndef BENCH_KERNELS_H
#define BENCH_KERNELS_H

#include <nativecall/kernel.h>

struct SumParams
{
    NC_U32 buf;         /* UBYTE * */
    NC_U32 len;
    NC_U32 init;        /* previous result: chunks chain */
};

struct XformParams
{
    NC_U32 matrix;      /* 16 floats, row major */
    NC_U32 in;          /* count x (x, y, z, w) */
    NC_U32 out;
    NC_U32 count;
};

/* *_neon / crc32_hw: NEON / ARMv8 CRC32 instructions natively, the plain version on the 68k */
#define BENCH_KERNELS(K) \
    K(adler32) K(adler32_neon) K(crc32_table) K(crc32_hw) K(xform) K(xform_neon)

NC_DECLARE(bench, BENCH_KERNELS)

#endif
