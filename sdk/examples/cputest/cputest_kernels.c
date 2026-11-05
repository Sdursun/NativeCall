/* cputest kernels: compiled for the Pi (cputest.narm) and for the 68k (fallback) */

#include "cputest_kernels.h"

#if NC_NATIVE
#include <arm_neon.h>
#endif

#define XMIN    (-2.0)
#define XMAX    (0.6)
#define YMIN    (-1.2)
#define YMAX    (1.2)

static int is_prime(NC_U32 n)
{
    NC_U32 d;

    if (n < 2)
        return 0;
    if ((n & 1) == 0)
        return n == 2;
    for (d = 3; d * d <= n; d += 2)
        if (n % d == 0)
            return 0;
    return 1;
}

NC_KERNEL(primes)
{
    struct CTParams *p = params;
    NC_U32 n, count = 0;

    for (n = p->range.from; n < p->range.to; n++)
        count += is_prime(n);
    return count;
}

/* Same doubles on both sides: the native side is built with -ffp-contract=off (Makefile) */
NC_KERNEL(mandel)
{
    struct CTParams *p = params;
    NC_U32 x, y, i, sum = 0;
    double dx = (XMAX - XMIN) / p->width, dy = (YMAX - YMIN) / p->height;

    for (y = p->range.from; y < p->range.to; y++)
    {
        double ci = YMIN + y * dy;

        for (x = 0; x < p->width; x++)
        {
            double cr = XMIN + x * dx;
            double zr = 0.0, zi = 0.0, zr2 = 0.0, zi2 = 0.0;

            for (i = 0; i < p->maxiter && zr2 + zi2 <= 4.0; i++)
            {
                zi = 2.0 * zr * zi + ci;
                zr = zr2 - zi2 + cr;
                zr2 = zr * zr;
                zi2 = zi * zi;
            }
            sum += i;
        }
    }
    return sum;
}

#if NC_NATIVE

NC_KERNEL(mandel_neon)
{
    struct CTParams *p = params;
    const float dx = (float)((XMAX - XMIN) / p->width);
    const float dy = (float)((YMAX - YMIN) / p->height);
    const float32x4_t four = vdupq_n_f32(4.0f);
    const float32x4_t lane = { 0.0f, 1.0f, 2.0f, 3.0f };
    uint32x4_t total = vdupq_n_u32(0);

    for (uint32_t y = p->range.from; y < p->range.to; y++)
    {
        float32x4_t ci = vdupq_n_f32((float)YMIN + y * dy);

        for (uint32_t x = 0; x < p->width; x += 4)
        {
            float32x4_t cr = vmlaq_n_f32(vdupq_n_f32((float)XMIN + x * dx), lane, dx);
            float32x4_t zr = vdupq_n_f32(0), zi = vdupq_n_f32(0);
            float32x4_t zr2 = vdupq_n_f32(0), zi2 = vdupq_n_f32(0);
            uint32x4_t count = vdupq_n_u32(0);
            uint32x4_t valid = vcltq_u32(vaddq_u32(vdupq_n_u32(x), (uint32x4_t){ 0, 1, 2, 3 }),
                                         vdupq_n_u32(p->width));

            for (uint32_t i = 0; i < p->maxiter; i++)
            {
                uint32x4_t inside = vandq_u32(vcleq_f32(vaddq_f32(zr2, zi2), four), valid);
                if (vmaxvq_u32(inside) == 0)
                    break;
                count = vsubq_u32(count, inside);       /* inside lanes are all ones (-1) */
                zi = vaddq_f32(vmulq_f32(vaddq_f32(zr, zr), zi), ci);
                zr = vaddq_f32(vsubq_f32(zr2, zi2), cr);
                zr2 = vbslq_f32(inside, vmulq_f32(zr, zr), zr2);
                zi2 = vbslq_f32(inside, vmulq_f32(zi, zi), zi2);
            }
            total = vaddq_u32(total, count);
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

NC_MODULE(cputest, CPUTEST_KERNELS)
