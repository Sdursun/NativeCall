/* mandeldemo kernels: compiled for the Pi (mandeldemo.narm) and for the 68k (fallback) */

#include "mandeldemo_kernels.h"

#if NC_NATIVE
#include <arm_neon.h>
#endif

/* palette index of an iteration count */
static NC_U32 color(const struct MandelParams *p, NC_U32 i)
{
    return i >= p->maxiter ? 0 : 1 + (i * 3 + p->shift) % MD_COLORS;
}

static void put(const struct MandelParams *p, NC_U32 offset, NC_U32 index)
{
    switch (p->bpp)
    {
        case 1: NC_PTR(NC_U8, p->out)[offset] = (NC_U8)index; break;
        case 2: NC_PTR(NC_U16, p->out)[offset] = NC_PTR(NC_U16, p->lut)[index]; break;
        default: NC_PTR(NC_U32, p->out)[offset] = NC_PTR(NC_U32, p->lut)[index]; break;
    }
}

NC_KERNEL(mandel)
{
    struct MandelParams *p = params;
    NC_U32 piece, x, i, sum = 0, seg_w = p->width / MD_SEGS;

    for (piece = p->range.from; piece < p->range.to; piece++)
    {
        NC_U32 y = piece / MD_SEGS, x_from = (piece % MD_SEGS) * seg_w;
        double ci = p->y0 + y * p->step;

        for (x = x_from; x < x_from + seg_w; x++)
        {
            double cr = p->x0 + x * p->step;
            double zr = 0.0, zi = 0.0, zr2 = 0.0, zi2 = 0.0;

            for (i = 0; i < p->maxiter && zr2 + zi2 <= 4.0; i++)
            {
                zi = 2.0 * zr * zi + ci;
                zr = zr2 - zi2 + cr;
                zr2 = zr * zr;
                zi2 = zi * zi;
            }
            sum += i;
            put(p, y * p->width + x, color(p, i));
        }
    }
    return sum;
}

#if NC_NATIVE

NC_KERNEL(mandel_neon)
{
    struct MandelParams *p = params;
    const float32x4_t four = vdupq_n_f32(4.0f);
    NC_U32 piece, seg_w = p->width / MD_SEGS;
    uint32x4_t total = vdupq_n_u32(0);

    for (piece = p->range.from; piece < p->range.to; piece++)
    {
        NC_U32 y = piece / MD_SEGS, x_from = (piece % MD_SEGS) * seg_w;
        float32x4_t ci = vdupq_n_f32((float)(p->y0 + y * p->step));

        for (NC_U32 x = x_from; x < x_from + seg_w; x += 4)
        {
            /* coordinates in double, then rounded once: no drift along the row */
            float c[4];
            NC_U32 n[4];

            for (int l = 0; l < 4; l++)
                c[l] = (float)(p->x0 + (x + l) * p->step);

            float32x4_t cr = vld1q_f32(c);
            float32x4_t zr = vdupq_n_f32(0), zi = vdupq_n_f32(0);
            float32x4_t zr2 = vdupq_n_f32(0), zi2 = vdupq_n_f32(0);
            uint32x4_t count = vdupq_n_u32(0);

            for (NC_U32 i = 0; i < p->maxiter; i++)
            {
                uint32x4_t inside = vcleq_f32(vaddq_f32(zr2, zi2), four);
                if (vmaxvq_u32(inside) == 0)
                    break;
                count = vsubq_u32(count, inside);       /* inside lanes are all ones (-1) */
                zi = vaddq_f32(vmulq_f32(vaddq_f32(zr, zr), zi), ci);
                zr = vaddq_f32(vsubq_f32(zr2, zi2), cr);
                zr2 = vbslq_f32(inside, vmulq_f32(zr, zr), zr2);
                zi2 = vbslq_f32(inside, vmulq_f32(zi, zi), zi2);
            }
            total = vaddq_u32(total, count);
            vst1q_u32(n, count);
            for (int l = 0; l < 4; l++)
                put(p, y * p->width + x + l, color(p, n[l]));
        }
    }
    return vaddvq_u32(total);
}

#else

NC_KERNEL(mandel_neon)
{
    return mandel(params);
}

#endif

NC_MODULE(mandeldemo, MANDELDEMO_KERNELS)
