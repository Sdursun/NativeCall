/* raydemo kernels: a small ray tracer, the same code on the 68k and natively */
#ifndef RAYDEMO_KERNELS_H
#define RAYDEMO_KERNELS_H

#include <nativecall/kernel.h>

#define RAY_SEGS    4       /* pieces per row: one native call stays short */
#define RAY_SPHERES 6
#define RAY_BOUNCES 3

struct RaySphere
{
    float x, y, z, r;
    float red, green, blue;     /* 0..1 */
    float reflect;              /* 0..1: how much of the colour is the mirror image */
};

/* Read by the kernel, written by the 68k between frames */
struct RayScene
{
    float eye[3];
    float right[3], up[3], forward[3];  /* camera axes, unit length */
    float light[3];                     /* position of the lamp */
    float focal;                        /* distance of the picture plane in pixels */
    struct RaySphere sphere[RAY_SPHERES];
};

struct RayParams
{
    struct NCRange range;   /* pieces: row * RAY_SEGS + segment */
    NC_U32 scene;           /* struct RayScene * */
    NC_U32 out;             /* the screen buffer */
    NC_U32 width, height;   /* width: a multiple of RAY_SEGS */
    NC_U32 fmt;             /* DFMT_* (demopix.h) */
    NC_U32 bpp;
};

NC_CHECK_SIZE(struct RayParams, 32)

/* trace: renders the pieces of the range, returns the number of rays followed */
#define RAYDEMO_KERNELS(K)  K(trace)

NC_DECLARE(raydemo, RAYDEMO_KERNELS)

#endif
