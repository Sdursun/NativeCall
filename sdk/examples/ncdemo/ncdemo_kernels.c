/* ncdemo kernels: compiled for the Pi (ncdemo.narm) and for the 68k (fallback) */

#include "ncdemo_kernels.h"

NC_KERNEL(transform)
{
    struct TransformParams *p = params;
    const struct Vertex *v = NC_PTR(const struct Vertex, p->vertices);
    struct SVertex *s = NC_PTR(struct SVertex, p->svertices);
    const struct Camera *c = NC_PTR(const struct Camera, p->camera);
    const float *r = c->rot;
    NC_U32 i;

    for (i = p->range.from; i < p->range.to; i++)
    {
        const struct Vertex *a = &v[i];
        float x = r[0] * a->x + r[1] * a->y + r[2] * a->z + c->trans[0];
        float y = r[3] * a->x + r[4] * a->y + r[5] * a->z + c->trans[1];
        float z = r[6] * a->x + r[7] * a->y + r[8] * a->z + c->trans[2];
        float nx = r[0] * a->nx + r[1] * a->ny + r[2] * a->nz;
        float ny = r[3] * a->nx + r[4] * a->ny + r[5] * a->nz;
        float nz = r[6] * a->nx + r[7] * a->ny + r[8] * a->nz;
        float d = -(nx * c->light[0] + ny * c->light[1] + nz * c->light[2]);
        float shade, spec;

        if (d < 0.0f)
            d = 0.0f;
        spec = d * d;
        spec = spec * spec;
        spec = spec * spec;                     /* d^8: a soft highlight */
        shade = 0.12f + 0.68f * d + 0.35f * spec;
        if (shade > 1.0f)
            shade = 1.0f;

        s[i].x = c->cx + c->focal * x / z;
        s[i].y = c->cy - c->focal * y / z;
        s[i].z = z;
        s[i].shade = shade;
    }
    return 0;
}

/* ceil(v - 0.5): first pixel whose centre is at or after v (no libm in kernels) */
static int first_pixel(float v)
{
    float f = v - 0.5f;
    int i = (int)f;             /* truncates towards zero */

    if ((float)i < f)
        i++;
    return i;
}

/*
    Scanline rasterizer with z-buffer and Gouraud shading, limited to the rows [y0, y1):
    every call clears and draws its own band, so nc_run can split the screen anywhere.
    Pixel centres at +0.5, spans from ceil(xl - 0.5) to ceil(xr - 0.5).
*/
static NC_U32 draw_triangle(const struct SVertex *v0, const struct SVertex *v1, const struct SVertex *v2,
                            NC_U32 color, int y0, int y1, int w, NC_U8 *chunky, float *zbuf)
{
    const struct SVertex *t;
    int y, ys, ye, x, xs, xe;
    NC_U32 drawn = 0;
    float area;

    /* back faces: front faces are clockwise on screen (y down) */
    area = (v1->x - v0->x) * (v2->y - v0->y) - (v2->x - v0->x) * (v1->y - v0->y);
    if (area >= 0.0f)
        return 0;

    if (v1->y < v0->y) { t = v0; v0 = v1; v1 = t; }
    if (v2->y < v0->y) { t = v0; v0 = v2; v2 = t; }
    if (v2->y < v1->y) { t = v1; v1 = v2; v2 = t; }
    if (v2->y <= v0->y)
        return 0;

    ys = first_pixel(v0->y);
    ye = first_pixel(v2->y);
    if (ys < y0) ys = y0;
    if (ye > y1) ye = y1;

    for (y = ys; y < ye; y++)
    {
        float sy = (float)y + 0.5f;
        float f = (sy - v0->y) / (v2->y - v0->y);
        float xl = v0->x + f * (v2->x - v0->x);
        float zl = v0->z + f * (v2->z - v0->z);
        float sl = v0->shade + f * (v2->shade - v0->shade);
        float xr, zr, sr, dz, ds, z, s, inv;

        if (sy < v1->y)
        {
            f = (sy - v0->y) / (v1->y - v0->y);
            xr = v0->x + f * (v1->x - v0->x);
            zr = v0->z + f * (v1->z - v0->z);
            sr = v0->shade + f * (v1->shade - v0->shade);
        }
        else
        {
            f = (sy - v1->y) / (v2->y - v1->y);
            xr = v1->x + f * (v2->x - v1->x);
            zr = v1->z + f * (v2->z - v1->z);
            sr = v1->shade + f * (v2->shade - v1->shade);
        }
        if (xr < xl)
        {
            float tmp;
            tmp = xl; xl = xr; xr = tmp;
            tmp = zl; zl = zr; zr = tmp;
            tmp = sl; sl = sr; sr = tmp;
        }
        if (xr - xl < 0.0001f)
            continue;

        xs = first_pixel(xl);
        xe = first_pixel(xr);
        if (xs < 0) xs = 0;
        if (xe > w) xe = w;

        inv = 1.0f / (xr - xl);
        dz = (zr - zl) * inv;
        ds = (sr - sl) * inv;
        z = zl + ((float)xs + 0.5f - xl) * dz;
        s = sl + ((float)xs + 0.5f - xl) * ds;

        {
            NC_U8 *pix = chunky + y * w;
            float *zrow = zbuf + y * w;

            for (x = xs; x < xe; x++, z += dz, s += ds)
            {
                if (z < zrow[x])
                {
                    int level = (int)(s * (float)RAMP_LEVELS);

                    if (level < 0) level = 0;
                    if (level > RAMP_LEVELS - 1) level = RAMP_LEVELS - 1;
                    zrow[x] = z;
                    pix[x] = (NC_U8)(color + level);
                    drawn++;
                }
            }
        }
    }
    return drawn;
}

NC_KERNEL(raster)
{
    struct RasterParams *p = params;
    const struct SVertex *sv = NC_PTR(const struct SVertex, p->svertices);
    const struct Triangle *tri = NC_PTR(const struct Triangle, p->triangles);
    NC_U8 *chunky = NC_PTR(NC_U8, p->chunky);
    float *zbuf = NC_PTR(float, p->zbuffer);
    int w = (int)p->width, y0 = (int)p->range.from, y1 = (int)p->range.to;
    float fy0 = (float)y0 + 0.5f, fy1 = (float)y1 - 0.5f;
    NC_U32 i, drawn = 0;
    int i2;

    /* clear the band */
    for (i2 = y0 * w; i2 < y1 * w; i2++)
    {
        chunky[i2] = (NC_U8)p->background;
        zbuf[i2] = 1e30f;
    }

    for (i = 0; i < p->ntriangles; i++)
    {
        const struct SVertex *a = &sv[tri[i].a], *b = &sv[tri[i].b], *c = &sv[tri[i].c];

        /* quick reject: triangle entirely above or below the band */
        if ((a->y < fy0 && b->y < fy0 && c->y < fy0) || (a->y > fy1 && b->y > fy1 && c->y > fy1))
            continue;
        drawn += draw_triangle(a, b, c, tri[i].color, y0, y1, w, chunky, zbuf);
    }
    return drawn;
}

/* The LUT holds each colour exactly as the screen stores it (byte order included), so this
   is a plain table lookup for any 16 or 32 bit format */
NC_KERNEL(present)
{
    struct PresentParams *p = params;
    const NC_U8 *src = NC_PTR(const NC_U8, p->chunky) + p->range.from * p->width;
    NC_U32 n = (p->range.to - p->range.from) * p->width, i;

    if (p->bpp == 2)
    {
        const NC_U16 *lut = NC_PTR(const NC_U16, p->lut);
        NC_U16 *dst = NC_PTR(NC_U16, p->out) + p->range.from * p->width;

        for (i = 0; i < n; i++)
            dst[i] = lut[src[i]];
    }
    else
    {
        const NC_U32 *lut = NC_PTR(const NC_U32, p->lut);
        NC_U32 *dst = NC_PTR(NC_U32, p->out) + p->range.from * p->width;

        for (i = 0; i < n; i++)
            dst[i] = lut[src[i]];
    }
    return 0;
}

NC_MODULE(ncdemo, NCDEMO_KERNELS)
