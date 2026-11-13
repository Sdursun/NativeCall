/* raydemo kernels: compiled for the Pi (raydemo.narm) and for the 68k (fallback) */

#include "raydemo_kernels.h"
#include "../common/demopix.h"
#include <nativecall/nc_math.h>

#define EPS     0.001f
#define FAR     1.0e30f

/* Nearest sphere the ray (origin o, unit direction d) hits, -1 if none; *t = its distance */
static int hit_sphere(const struct RayScene *sc, const float *o, const float *d, float *t)
{
    float best = FAR;
    int which = -1, i;

    for (i = 0; i < RAY_SPHERES; i++)
    {
        const struct RaySphere *s = &sc->sphere[i];
        float x = o[0] - s->x, y = o[1] - s->y, z = o[2] - s->z;
        float b = x * d[0] + y * d[1] + z * d[2];
        float c = x * x + y * y + z * z - s->r * s->r;
        float disc = b * b - c;

        if (disc > 0.0f)
        {
            float dist = -b - nc_sqrtf(disc);

            if (dist > EPS && dist < best)
            {
                best = dist;
                which = i;
            }
        }
    }
    *t = best;
    return which;
}

/* Is something between the point o and the lamp (direction d, distance max)? */
static int in_shadow(const struct RayScene *sc, const float *o, const float *d, float max)
{
    int i;

    for (i = 0; i < RAY_SPHERES; i++)
    {
        const struct RaySphere *s = &sc->sphere[i];
        float x = o[0] - s->x, y = o[1] - s->y, z = o[2] - s->z;
        float b = x * d[0] + y * d[1] + z * d[2];
        float c = x * x + y * y + z * z - s->r * s->r;
        float disc = b * b - c;

        if (disc > 0.0f)
        {
            float dist = -b - nc_sqrtf(disc);

            if (dist > EPS && dist < max)
                return 1;
        }
    }
    return 0;
}

/* Colour seen along one ray: follows it over up to RAY_BOUNCES mirrors. Returns the rays used. */
static NC_U32 shade(const struct RayScene *sc, float *o, float *d, float *rgb)
{
    float weight = 1.0f, r = 0.0f, g = 0.0f, b = 0.0f;
    NC_U32 rays = 0;
    int bounce;

    for (bounce = 0; bounce < RAY_BOUNCES; bounce++)
    {
        float t, tp = FAR, p[3], n[3], l[3], cr, cg, cb, refl, dist, diff, spec = 0.0f, dn, k, fade = 1.0f, haze;
        int s = hit_sphere(sc, o, d, &t);

        rays++;
        if (d[1] < -EPS)
            tp = -o[1] / d[1];                      /* the floor: y = 0 */
        if (s < 0 && tp >= FAR)
        {
            /* sky: lighter towards the horizon */
            float h = d[1] < 0.0f ? 0.0f : d[1];

            k = 1.0f - h;
            k = k * k;
            r += weight * (0.25f + 0.60f * k);
            g += weight * (0.45f + 0.50f * k);
            b += weight * (0.85f + 0.15f * k);
            break;
        }
        if (s >= 0 && t < tp)
        {
            const struct RaySphere *sp = &sc->sphere[s];
            float inv = 1.0f / sp->r;

            p[0] = o[0] + t * d[0];
            p[1] = o[1] + t * d[1];
            p[2] = o[2] + t * d[2];
            n[0] = (p[0] - sp->x) * inv;
            n[1] = (p[1] - sp->y) * inv;
            n[2] = (p[2] - sp->z) * inv;
            cr = sp->red;
            cg = sp->green;
            cb = sp->blue;
            refl = sp->reflect;
        }
        else
        {
            float fx, fz;
            int check;

            t = tp;
            p[0] = o[0] + t * d[0];
            p[1] = 0.0f;
            p[2] = o[2] + t * d[2];
            n[0] = 0.0f;
            n[1] = 1.0f;
            n[2] = 0.0f;
            fx = nc_floorf(p[0] * 0.5f);
            fz = nc_floorf(p[2] * 0.5f);
            check = ((int)fx + (int)fz) & 1;
            /* far away the floor disappears in the haze (mixed in after the light, below) */
            fade = 1.0f / (1.0f + t * t * 0.0006f);
            cr = check ? 0.92f : 0.20f;
            cg = check ? 0.90f : 0.22f;
            cb = check ? 0.84f : 0.28f;
            refl = 0.28f * fade;
        }

        /* the lamp */
        l[0] = sc->light[0] - p[0];
        l[1] = sc->light[1] - p[1];
        l[2] = sc->light[2] - p[2];
        dist = nc_sqrtf(l[0] * l[0] + l[1] * l[1] + l[2] * l[2]);
        k = 1.0f / dist;
        l[0] *= k;
        l[1] *= k;
        l[2] *= k;
        diff = n[0] * l[0] + n[1] * l[1] + n[2] * l[2];

        /* mirror direction */
        dn = d[0] * n[0] + d[1] * n[1] + d[2] * n[2];
        d[0] -= 2.0f * dn * n[0];
        d[1] -= 2.0f * dn * n[1];
        d[2] -= 2.0f * dn * n[2];
        o[0] = p[0] + EPS * n[0];
        o[1] = p[1] + EPS * n[1];
        o[2] = p[2] + EPS * n[2];

        if (diff > 0.0f && !in_shadow(sc, o, l, dist))
        {
            rays++;
            spec = d[0] * l[0] + d[1] * l[1] + d[2] * l[2];
            if (spec > 0.0f)
            {
                spec = spec * spec;
                spec = spec * spec;
                spec = spec * spec;
                spec = spec * spec;
                spec = spec * spec * 0.7f;      /* ^32: a small highlight */
            }
            else
                spec = 0.0f;
        }
        else
        {
            rays++;
            diff = 0.0f;
        }
        k = weight * (1.0f - refl) * (0.16f + 0.84f * diff);
        haze = weight * (1.0f - refl) * (1.0f - fade);
        r += (k * cr + weight * spec) * fade + haze * 0.72f;
        g += (k * cg + weight * spec) * fade + haze * 0.86f;
        b += (k * cb + weight * spec) * fade + haze * 0.98f;

        weight *= refl;
        if (weight < 0.02f)
            break;
    }
    rgb[0] = r;
    rgb[1] = g;
    rgb[2] = b;
    return rays;
}

static NC_U32 to_byte(float v)
{
    int i = (int)(v * 255.0f);

    return (NC_U32)(i < 0 ? 0 : i > 255 ? 255 : i);
}

NC_KERNEL(trace)
{
    struct RayParams *p = params;
    const struct RayScene *sc = NC_PTR(const struct RayScene, p->scene);
    NC_U8 *out = NC_PTR(NC_U8, p->out);
    NC_U32 width = p->width, seg = width / RAY_SEGS, piece, rays = 0;
    float half_w = 0.5f * (float)width, half_h = 0.5f * (float)p->height;
    NC_U32 buf[64];         /* small: the 68k version runs on the stack of the program */

    for (piece = p->range.from; piece < p->range.to; piece++)
    {
        NC_U32 y = piece / RAY_SEGS, x0 = (piece % RAY_SEGS) * seg, x1 = x0 + seg, x, n = 0;
        float v = half_h - ((float)y + 0.5f);
        NC_U8 *dst = out + (y * width + x0) * p->bpp;

        for (x = x0; x < x1; x++)
        {
            float u = ((float)x + 0.5f) - half_w, o[3], d[3], rgb[3], k;

            d[0] = sc->forward[0] * sc->focal + sc->right[0] * u + sc->up[0] * v;
            d[1] = sc->forward[1] * sc->focal + sc->right[1] * u + sc->up[1] * v;
            d[2] = sc->forward[2] * sc->focal + sc->right[2] * u + sc->up[2] * v;
            k = 1.0f / nc_sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            d[0] *= k;
            d[1] *= k;
            d[2] *= k;
            o[0] = sc->eye[0];
            o[1] = sc->eye[1];
            o[2] = sc->eye[2];
            rays += shade(sc, o, d, rgb);
            buf[n++] = DEMO_RGB(to_byte(rgb[0]), to_byte(rgb[1]), to_byte(rgb[2]));
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
    return rays;
}

NC_MODULE(raydemo, RAYDEMO_KERNELS)
