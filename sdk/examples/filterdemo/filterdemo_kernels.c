/* filterdemo kernels: compiled for the Pi (filterdemo.narm) and for the 68k (fallback) */

#include "filterdemo_kernels.h"
#include "../common/demopix.h"

/* a + (b - a) * f / 256 for the three colours of a pixel at once (red and blue together) */
static NC_U32 mix(NC_U32 a, NC_U32 b, NC_U32 f)
{
    NC_U32 rb = (((a & 0xff00ff) * (256 - f) + (b & 0xff00ff) * f) >> 8) & 0xff00ff;
    NC_U32 g = (((a & 0x00ff00) * (256 - f) + (b & 0x00ff00) * f) >> 8) & 0x00ff00;

    return rb | g;
}

NC_KERNEL(rotozoom)
{
    struct RotoParams *p = params;
    const NC_U32 *src = NC_PTR(const NC_U32, p->src);
    NC_U32 *work = NC_PTR(NC_U32, p->work);
    NC_U32 width = p->width, y, x;

    for (y = p->range.from; y < p->range.to; y++)
    {
        NC_S32 u = p->u0 + (NC_S32)y * p->dudy, v = p->v0 + (NC_S32)y * p->dvdy;
        NC_U32 *dst = work + y * width;

        for (x = 0; x < width; x++, u += p->dudx, v += p->dvdx)
        {
            NC_U32 ui = ((NC_U32)u >> 16) & SRC_MASK, vi = ((NC_U32)v >> 16) & SRC_MASK;
            NC_U32 ui1 = (ui + 1) & SRC_MASK, vi1 = (vi + 1) & SRC_MASK;
            NC_U32 fu = ((NC_U32)u >> 8) & 0xff, fv = ((NC_U32)v >> 8) & 0xff;
            NC_U32 top = mix(src[vi * SRC_SIZE + ui], src[vi * SRC_SIZE + ui1], fu);
            NC_U32 bottom = mix(src[vi1 * SRC_SIZE + ui], src[vi1 * SRC_SIZE + ui1], fu);

            dst[x] = mix(top, bottom, fv);
        }
    }
    return 0;
}

static NC_U32 clamp255(NC_S32 v)
{
    return (NC_U32)(v < 0 ? 0 : v > 255 ? 255 : v);
}

NC_KERNEL(filter)
{
    struct FilterParams *p = params;
    const NC_U32 *work = NC_PTR(const NC_U32, p->work);
    NC_U8 *out = NC_PTR(NC_U8, p->out);
    NC_U32 width = p->width, height = p->height, y, x, n;
    NC_S32 w0 = p->weight[0], w1 = p->weight[1], w2 = p->weight[2], w3 = p->weight[3], w4 = p->weight[4],
           w5 = p->weight[5], w6 = p->weight[6], w7 = p->weight[7], w8 = p->weight[8];
    NC_S32 shift = p->shift, bias = p->bias;
    NC_U32 buf[64];         /* small: the 68k version runs on the stack of the program */

    for (y = p->range.from; y < p->range.to; y++)
    {
        /* the rows above and below; at the edges the picture's own edge again */
        const NC_U32 *r1 = work + y * width;
        const NC_U32 *r0 = y > 0 ? r1 - width : r1;
        const NC_U32 *r2 = y + 1 < height ? r1 + width : r1;
        NC_U8 *dst = out + y * width * p->bpp;

        n = 0;
        for (x = 0; x < width; x++)
        {
            NC_U32 xl = x > 0 ? x - 1 : x, xr = x + 1 < width ? x + 1 : x;
            NC_U32 a = r0[xl], b = r0[x], c = r0[xr], d = r1[xl], e = r1[x], f = r1[xr],
                   g = r2[xl], h = r2[x], i = r2[xr];
            NC_S32 red, green, blue;

#define CH(px, s)   ((NC_S32)(((px) >> (s)) & 0xff))
#define SUM(s)      (w0 * CH(a, s) + w1 * CH(b, s) + w2 * CH(c, s) + w3 * CH(d, s) + w4 * CH(e, s) \
                     + w5 * CH(f, s) + w6 * CH(g, s) + w7 * CH(h, s) + w8 * CH(i, s))
            red = (SUM(16) >> shift) + bias;
            green = (SUM(8) >> shift) + bias;
            blue = (SUM(0) >> shift) + bias;
#undef SUM
#undef CH
            buf[n++] = DEMO_RGB(clamp255(red), clamp255(green), clamp255(blue));
            if (n == 64)
            {
                demo_store(dst, buf, n, p->fmt);
                dst += n * p->bpp;
                n = 0;
            }
        }
        if (n)
            demo_store(dst, buf, n, p->fmt);
    }
    return 0;
}

NC_MODULE(filterdemo, FILTERDEMO_KERNELS)
