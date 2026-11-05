/* mandeldemo kernels: a Mandelbrot zoom, the same code on the 68k and natively */
#ifndef MANDELDEMO_KERNELS_H
#define MANDELDEMO_KERNELS_H

#include <nativecall/kernel.h>

#define MD_SEGS     4       /* pieces per row: one native call stays short on deep rows */

struct MandelParams
{
    struct NCRange range;   /* pieces: row * MD_SEGS + segment */
    double x0, y0;          /* complex coordinates of the top left pixel */
    double step;            /* complex units per pixel */
    NC_U32 width, height;   /* width: a multiple of 4 * MD_SEGS */
    NC_U32 maxiter;
    NC_U32 shift;           /* colour cycling */
    NC_U32 out;             /* width * height pixels, bpp bytes each */
    NC_U32 lut;             /* 256 pixels in the screen's format (bpp 2 or 4) */
    NC_U32 bpp;             /* 1: palette indexes (no lut), 2 or 4: screen pixels */
    NC_U32 reserved;
};

NC_CHECK_SIZE(struct MandelParams, 64)

#define MD_COLORS   253     /* palette 1..253: colour ramp; 0: inside the set */

/* mandel:      double precision, returns the sum of the iteration counts
   mandel_neon: natively 4 pixels at a time in single precision (NEON);
                on the 68k it is the double version */
#define MANDELDEMO_KERNELS(K)   K(mandel) K(mandel_neon)

NC_DECLARE(mandeldemo, MANDELDEMO_KERNELS)

#endif
