/* voxeldemo kernels: compiled for the Pi (voxeldemo.narm) and for the 68k (fallback) */

#include "voxeldemo_kernels.h"
#include "../common/demopix.h"

/* The land is a sum of waves with whole numbers of periods over the map, so it repeats without
   a seam. { periods along x, periods along y, phase, amplitude } */
static const short waves[WAVES][4] = {
    {  1,  2,  100, 60 }, {  3, -1,  420, 42 }, {  2,  5,  810, 30 }, { -6,  3,  250, 20 },
    {  9,  7,  630, 13 }, { 13, -9,   70,  9 }, { 21, 15,  940,  6 }, {-29, 23,  380,  4 },
    { 47, 31,  555,  3 }, { 61,-53,  190,  2 },
};

static NC_U32 clamp255(int v)
{
    return (NC_U32)(v < 0 ? 0 : v > 255 ? 255 : v);
}

NC_KERNEL(terrain)
{
    struct TerrainParams *p = params;
    NC_U8 *hm = NC_PTR(NC_U8, p->heights);
    NC_U32 *cm = NC_PTR(NC_U32, p->colors);
    const float *sine = NC_PTR(const float, p->sine);
    NC_U32 y, x;
    int i;

    for (y = p->range.from; y < p->range.to; y++)
    {
        if (p->pass == 0)
        {
            for (x = 0; x < MAP_SIZE; x++)
            {
                float h = 0.0f;
                int v;

                for (i = 0; i < WAVES; i++)
                    h += (float)waves[i][3] * NC_GETF(&sine[(NC_U32)(waves[i][0] * (int)x + waves[i][1] * (int)y + waves[i][2]) & MAP_MASK]);
                /* valleys become wide, peaks sharp */
                h = h * (1.0f / 189.0f);                /* -1..1 */
                h = 0.5f + 0.5f * h;
                h = h * h * 250.0f;
                v = (int)h;
                if (v < WATER)
                    v = WATER;
                hm[y * MAP_SIZE + x] = (NC_U8)(v > 255 ? 255 : v);
            }
        }
        else
        {
            for (x = 0; x < MAP_SIZE; x++)
            {
                int h = hm[y * MAP_SIZE + x];
                /* light from the west and a little from the north */
                int slope = (int)hm[y * MAP_SIZE + ((x + 1) & MAP_MASK)] - (int)hm[y * MAP_SIZE + ((x - 1) & MAP_MASK)]
                          + ((int)hm[((y + 1) & MAP_MASK) * MAP_SIZE + x] - (int)hm[((y - 1) & MAP_MASK) * MAP_SIZE + x]) / 2;
                int light = 256 + slope * 22, r, g, b;

                if (light < 96) light = 96;
                if (light > 420) light = 420;
                if (h <= WATER)
                {
                    /* sea: ripples from two of the waves */
                    int ripple = (int)(24.0f * NC_GETF(&sine[(NC_U32)(37 * (int)x + 11 * (int)y) & MAP_MASK]));

                    r = 24;
                    g = 78 + ripple / 2;
                    b = 150 + ripple;
                    light = 256;
                }
                else if (h < WATER + 5)      { r = 214; g = 196; b = 140; }    /* beach */
                else if (h < 120)            { r = 60 + (h - WATER) / 2; g = 132 - (h - WATER) / 3; b = 44; }
                else if (h < 170)            { r = 112 + (h - 120) / 2; g = 100 + (h - 120) / 3; b = 78; }
                else                         { r = 232; g = 236; b = 244; }    /* snow */
                NC_PUT32(&cm[y * MAP_SIZE + x], DEMO_RGB(clamp255(r * light >> 8), clamp255(g * light >> 8),
                                                clamp255(b * light >> 8)));
            }
        }
    }
    return 0;
}

NC_KERNEL(voxel)
{
    struct VoxelParams *p = params;
    const NC_U8 *hm = NC_PTR(const NC_U8, p->heights);
    const NC_U32 *cm = NC_PTR(const NC_U32, p->colors);
    const NC_U32 *sky = NC_PTR(const NC_U32, p->sky);
    NC_U8 *out = NC_PTR(NC_U8, p->out);
    NC_U32 width = p->width, bpp = p->bpp, stride = width * bpp, x, drawn = 0;
    int height = (int)p->height;
    int hr = (int)((p->haze >> 16) & 0xff), hg = (int)((p->haze >> 8) & 0xff), hb = (int)(p->haze & 0xff);
    float half = 0.5f * (float)width, inv_dist = 256.0f / p->distance;

    for (x = p->range.from; x < p->range.to; x++)
    {
        /* this column's ray over the map: forward plus its share of "right" */
        float side = (((float)x + 0.5f) - half) / half * p->fov;
        float dx = p->cosa - side * p->sina, dy = p->sina + side * p->cosa;
        float z = 2.0f, dz = 1.0f;
        int top = height, y;                /* everything from 'top' down is drawn */
        NC_U8 *col = out + x * bpp;

        while (z < p->distance && top > 0)
        {
            NC_U32 mx = (NC_U32)(int)(p->x + dx * z) & MAP_MASK;
            NC_U32 my = (NC_U32)(int)(p->y + dy * z) & MAP_MASK;
            NC_U32 at = my * MAP_SIZE + mx;
            int sy = (int)((p->altitude - (float)hm[at]) * p->scale / z + p->horizon);

            if (sy < 0)
                sy = 0;
            if (sy < top)
            {
                NC_U32 c = NC_GET32(&cm[at]), packed;
                int f = (int)(z * inv_dist);        /* 0..256: how much haze */
                int r = (int)((c >> 16) & 0xff), g = (int)((c >> 8) & 0xff), b = (int)(c & 0xff);
                NC_U8 *dst = col + (NC_U32)sy * stride;

                f = f * f >> 8;
                r += (hr - r) * f >> 8;
                g += (hg - g) * f >> 8;
                b += (hb - b) * f >> 8;
                packed = demo_pack(DEMO_RGB(r, g, b), p->fmt);
                drawn += (NC_U32)(top - sy);
                for (y = sy; y < top; y++, dst += stride)
                    demo_put(dst, packed, bpp);
                top = sy;
            }
            z += dz;
            dz += 0.012f;                   /* far away the steps may be longer */
        }
        /* the sky above the land */
        {
            NC_U8 *dst = col;

            for (y = 0; y < top; y++, dst += stride)
                demo_put(dst, NC_GET32(&sky[y]), bpp);
        }
    }
    return drawn;
}

NC_MODULE(voxeldemo, VOXELDEMO_KERNELS)
