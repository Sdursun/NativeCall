/* nbodydemo kernels: gravity between all pairs of bodies, the same code on the 68k and natively */
#ifndef NBODYDEMO_KERNELS_H
#define NBODYDEMO_KERNELS_H

#include <nativecall/kernel.h>

struct Body
{
    float x, y, z;
    float vx, vy, vz;
    float mass;
    NC_U32 color;           /* 0x00RRGGBB */
};

NC_CHECK_SIZE(struct Body, 32)

/* One time step: reads all bodies of `from`, writes the bodies of the range to `to` (two
   buffers, so the pieces do not depend on each other) */
struct StepParams
{
    struct NCRange range;   /* bodies */
    NC_U32 from, to;        /* struct Body[count] */
    NC_U32 count;
    float dt;
    float soft;             /* softening length squared: no infinite force between close bodies */
    NC_U32 reserved;
};

struct DrawParams
{
    struct NCRange range;   /* screen rows */
    NC_U32 bodies, count;
    NC_U32 out;             /* the screen buffer */
    NC_U32 width, height;
    NC_U32 fmt, bpp;
    float scale;            /* pixels per unit */
    float cosa, sina;       /* the view turns around the vertical axis */
    float tilt_cos, tilt_sin;
};

NC_CHECK_SIZE(struct StepParams, 32)
NC_CHECK_SIZE(struct DrawParams, 56)

/* step: returns 0
   draw: clears the rows of the range and draws the bodies in them, returns the bodies drawn */
#define NBODYDEMO_KERNELS(K)    K(step) K(draw)

NC_DECLARE(nbodydemo, NBODYDEMO_KERNELS)

#endif
