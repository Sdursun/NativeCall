/* filterdemo kernels: rotate, scale and filter a picture, the same code on the 68k and natively */
#ifndef FILTERDEMO_KERNELS_H
#define FILTERDEMO_KERNELS_H

#include <nativecall/kernel.h>

#define SRC_BITS    9
#define SRC_SIZE    (1 << SRC_BITS)     /* the picture is SRC_SIZE x SRC_SIZE and repeats */
#define SRC_MASK    (SRC_SIZE - 1)

/* Step 1: the picture, turned and scaled with bilinear filtering, into a work buffer.
   Picture coordinates are 16.16 fixed point: pixel (x, y) of the screen shows
   (u0 + x * dudx + y * dudy, v0 + x * dvdx + y * dvdy). */
struct RotoParams
{
    struct NCRange range;   /* screen rows */
    NC_U32 src;             /* NC_U32[SRC_SIZE * SRC_SIZE], 0x00RRGGBB */
    NC_U32 work;            /* NC_U32[width * height], 0x00RRGGBB */
    NC_U32 width, height;
    NC_S32 u0, v0, dudx, dvdx, dudy, dvdy;
};

/* Step 2: a 3 x 3 filter over the work buffer, onto the screen:
   pixel = (sum of weight * neighbour >> shift) + bias, each colour on its own */
struct FilterParams
{
    struct NCRange range;   /* screen rows */
    NC_U32 work;
    NC_U32 out;             /* the screen buffer */
    NC_U32 width, height;
    NC_U32 fmt, bpp;
    NC_S32 weight[9];
    NC_S32 shift, bias;
};

NC_CHECK_SIZE(struct RotoParams, 48)
NC_CHECK_SIZE(struct FilterParams, 76)

#define FILTERDEMO_KERNELS(K)   K(rotozoom) K(filter)

NC_DECLARE(filterdemo, FILTERDEMO_KERNELS)

#endif
