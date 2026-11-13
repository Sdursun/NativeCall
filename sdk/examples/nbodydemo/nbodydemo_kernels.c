/* nbodydemo kernels: compiled for the Pi (nbodydemo.narm) and for the 68k (fallback) */

#include "nbodydemo_kernels.h"
#include "../common/demopix.h"
#include <nativecall/nc_math.h>

NC_KERNEL(step)
{
    struct StepParams *p = params;
    const struct Body *from = NC_PTR(const struct Body, p->from);
    struct Body *to = NC_PTR(struct Body, p->to);
    NC_U32 count = p->count, i, j;
    float dt = p->dt, soft = p->soft;

    for (i = p->range.from; i < p->range.to; i++)
    {
        const struct Body *a = &from[i];
        float ax = 0.0f, ay = 0.0f, az = 0.0f;

        for (j = 0; j < count; j++)
        {
            const struct Body *b = &from[j];
            float dx = b->x - a->x, dy = b->y - a->y, dz = b->z - a->z;
            float r2 = dx * dx + dy * dy + dz * dz + soft;      /* never 0, also for j == i */
            float inv = 1.0f / nc_sqrtf(r2);
            float f = b->mass * inv * inv * inv;

            ax += dx * f;
            ay += dy * f;
            az += dz * f;
        }
        to[i].vx = a->vx + ax * dt;
        to[i].vy = a->vy + ay * dt;
        to[i].vz = a->vz + az * dt;
        to[i].x = a->x + to[i].vx * dt;
        to[i].y = a->y + to[i].vy * dt;
        to[i].z = a->z + to[i].vz * dt;
        to[i].mass = a->mass;
        to[i].color = a->color;
    }
    return 0;
}

NC_KERNEL(draw)
{
    struct DrawParams *p = params;
    const struct Body *b = NC_PTR(const struct Body, p->bodies);
    NC_U8 *out = NC_PTR(NC_U8, p->out);
    NC_U32 width = p->width, bpp = p->bpp, i, n, drawn = 0;
    int y0 = (int)p->range.from, y1 = (int)p->range.to, w = (int)width;
    float cx = 0.5f * (float)width, cy = 0.5f * (float)p->height;
    NC_U8 *row = out + (NC_U32)y0 * width * bpp;

    /* black sky (the width is a multiple of 4 pixels, so a row is whole 32 bit words) */
    n = (NC_U32)(y1 - y0) * width * bpp / 4;
    for (i = 0; i < n; i++)
        ((NC_U32 *)row)[i] = 0;

    for (i = 0; i < p->count; i++)
    {
        /* turn around the vertical axis, then tilt towards the viewer */
        float x = b[i].x * p->cosa + b[i].z * p->sina;
        float z = b[i].z * p->cosa - b[i].x * p->sina;
        float y = b[i].y * p->tilt_cos + z * p->tilt_sin;
        int sx = (int)(cx + x * p->scale), sy = (int)(cy - y * p->scale), size, xx, yy;
        NC_U32 packed;

        size = b[i].mass > 50.0f ? 4 : b[i].mass > 1.5f ? 2 : 1;
        if (sy + size <= y0 || sy >= y1 || sx < 0 || sx + size > w)
            continue;
        packed = demo_pack(b[i].color, p->fmt);
        for (yy = sy; yy < sy + size; yy++)
        {
            if (yy < y0 || yy >= y1)
                continue;                   /* the rest belongs to another piece */
            for (xx = sx; xx < sx + size; xx++)
                demo_put(out + ((NC_U32)yy * width + (NC_U32)xx) * bpp, packed, bpp);
        }
        drawn++;
    }
    return drawn;
}

NC_MODULE(nbodydemo, NBODYDEMO_KERNELS)
