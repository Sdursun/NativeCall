/*
    raydemo - a ray tracer in real time: mirror spheres over a chequered floor, one lamp, hard
    shadows, up to three reflections. Every pixel is independent work, so the ARM cores share
    a frame row by row. Example for the nativecall SDK.

        raydemo [WIDTH=<w>] [HEIGHT=<h>] [SECONDS=<s>] [AUTO=<s>] [MODE=<1-3>]

    Keys and arguments: ../common/demofw.h. The scene depends only on the time: every mode
    shows the same picture at the same moment.
*/

#include <math.h>
#include <stdio.h>

#include "../common/demofw.h"
#include "raydemo_kernels.h"

static struct RayParams *rp;
static struct RayScene *scene;

static const struct RaySphere spheres[RAY_SPHERES] = {
    /*  x      y     z     r     red    green  blue   reflect */
    {  0.0f, 1.6f,  0.0f, 1.6f, 0.95f, 0.95f, 1.00f, 0.75f },     /* the big mirror */
    {  0.0f, 0.7f,  0.0f, 0.7f, 0.95f, 0.20f, 0.15f, 0.25f },     /* red, circles around it */
    {  0.0f, 0.7f,  0.0f, 0.7f, 0.20f, 0.80f, 0.25f, 0.25f },     /* green */
    {  0.0f, 0.7f,  0.0f, 0.7f, 0.20f, 0.35f, 0.95f, 0.25f },     /* blue */
    {  0.0f, 0.5f,  0.0f, 0.5f, 0.98f, 0.82f, 0.20f, 0.45f },     /* gold, bounces */
    {  0.0f, 0.4f,  0.0f, 0.4f, 0.85f, 0.30f, 0.90f, 0.10f },     /* violet, far circle */
};

static void normalize(float *v)
{
    float k = 1.0f / (float)sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);

    v[0] *= k;
    v[1] *= k;
    v[2] *= k;
}

static BOOL init(struct Demo *d)
{
    int i;

    rp = nc_alloc(sizeof(*rp));
    scene = nc_alloc(sizeof(*scene));
    if (!rp || !scene)
        return FALSE;
    for (i = 0; i < RAY_SPHERES; i++)
        scene->sphere[i] = spheres[i];
    rp->scene = (NC_U32)scene;
    rp->out = (NC_U32)d->pixels;
    rp->width = d->width;
    rp->height = d->height;
    rp->fmt = d->fmt;
    rp->bpp = d->bpp;
    return TRUE;
}

static void frame(struct Demo *d, int mode)
{
    double t = d->time, a;
    float *f = scene->forward, *r = scene->right, *u = scene->up;
    NC_U32 rays;
    int i;

    /* three spheres circle the mirror, one bounces, one runs a wide circle the other way */
    for (i = 1; i <= 3; i++)
    {
        a = 0.7 * t + i * 2.0943951;
        scene->sphere[i].x = (float)(3.2 * cos(a));
        scene->sphere[i].z = (float)(3.2 * sin(a));
    }
    a = fmod(1.3 * t, 1.0);
    scene->sphere[4].x = (float)(5.0 * cos(0.31 * t));
    scene->sphere[4].z = (float)(5.0 * sin(0.31 * t));
    scene->sphere[4].y = (float)(0.5 + 9.0 * a * (1.0 - a));
    scene->sphere[5].x = (float)(6.5 * cos(-0.45 * t + 1.0));
    scene->sphere[5].z = (float)(6.5 * sin(-0.45 * t + 1.0));

    /* the camera circles the scene and looks at the mirror */
    a = 0.17 * t;
    scene->eye[0] = (float)(11.0 * cos(a));
    scene->eye[1] = (float)(3.6 + 1.6 * sin(0.23 * t));
    scene->eye[2] = (float)(11.0 * sin(a));
    f[0] = -scene->eye[0];
    f[1] = 1.3f - scene->eye[1];
    f[2] = -scene->eye[2];
    normalize(f);
    r[0] = -f[2];                   /* forward x world up, level with the floor */
    r[1] = 0.0f;
    r[2] = f[0];
    normalize(r);
    u[0] = r[1] * f[2] - r[2] * f[1];
    u[1] = r[2] * f[0] - r[0] * f[2];
    u[2] = r[0] * f[1] - r[1] * f[0];
    scene->focal = 0.9f * (float)d->width;

    scene->light[0] = (float)(7.0 * cos(0.5 * t + 2.0));
    scene->light[1] = 9.0f;
    scene->light[2] = (float)(7.0 * sin(0.5 * t + 2.0));

    rays = demo_run(d, mode, NCK_trace, rp, sizeof(*rp), 0, d->height * RAY_SEGS);
    sprintf(d->info, "%lu rays", (unsigned long)rays);
}

static void cleanup(struct Demo *d)
{
    (void)d;
    nc_free(scene, sizeof(*scene));
    nc_free(rp, sizeof(*rp));
}

static const struct DemoApp app = {
    "raydemo", "ray tracer, 6 spheres, shadows, up to 3 reflections",
    640, 480, RAY_SEGS, init, frame, NULL, cleanup, NULL
};

int main(void)
{
    return demo_main(&app, &raydemo_kernels);
}

#include "../common/demofw.c"
