/*
    voxeldemo - a flight over a landscape drawn from a height map: every screen column casts one
    ray over the map and draws the land it meets from near to far. The columns do not depend on
    each other, so the ARM cores share the screen. The map (1024 x 1024, sea, beaches, hills,
    snow) is built by a kernel too when the program starts. Example for the nativecall SDK.

        voxeldemo [WIDTH=<w>] [HEIGHT=<h>] [SECONDS=<s>] [AUTO=<s>] [MODE=<1-3>]

    Keys and arguments: ../common/demofw.h.
*/

#include <math.h>
#include <stdio.h>

#include "../common/demofw.h"
#include "voxeldemo_kernels.h"

#define PI 3.14159265358979

static struct VoxelParams *vp;
static struct TerrainParams *tp;
static NC_U8 *heights;
static NC_U32 *colors, *sky;
static float *sine;

static BOOL init(struct Demo *d)
{
    ULONG y, i;
    int mode = nc_is_native(d->k) ? (d->cores > 1 ? DEMO_NATIVE_MP : DEMO_NATIVE) : DEMO_68K;

    vp = nc_alloc(sizeof(*vp));
    tp = nc_alloc(sizeof(*tp));
    heights = nc_alloc(MAP_SIZE * MAP_SIZE);
    colors = nc_alloc(MAP_SIZE * MAP_SIZE * 4);
    sine = nc_alloc(MAP_SIZE * sizeof(float));
    sky = nc_alloc(d->height * 4);
    if (!vp || !tp || !heights || !colors || !sine || !sky)
        return FALSE;

    /* the map, by the fastest way there is */
    for (i = 0; i < MAP_SIZE; i++)
        sine[i] = (float)sin(2 * PI * i / MAP_SIZE);
    tp->heights = (NC_U32)heights;
    tp->colors = (NC_U32)colors;
    tp->sine = (NC_U32)sine;
    tp->pass = 0;
    demo_run(d, mode, NCK_terrain, tp, sizeof(*tp), 0, MAP_SIZE);
    tp->pass = 1;
    demo_run(d, mode, NCK_terrain, tp, sizeof(*tp), 0, MAP_SIZE);

    /* the sky: deep blue at the top, pale at the horizon */
    for (y = 0; y < d->height; y++)
    {
        ULONG k = y * 512 / d->height;

        if (k > 255)
            k = 255;
        sky[y] = demo_pack(DEMO_RGB(40 + k * 150 / 255, 96 + k * 124 / 255, 200 + k * 50 / 255), d->fmt);
    }

    vp->heights = (NC_U32)heights;
    vp->colors = (NC_U32)colors;
    vp->out = (NC_U32)d->pixels;
    vp->sky = (NC_U32)sky;
    vp->width = d->width;
    vp->height = d->height;
    vp->fmt = d->fmt;
    vp->bpp = d->bpp;
    vp->fov = 0.66f;
    vp->scale = 0.75f * (float)d->height;
    vp->distance = 700.0f;
    vp->haze = DEMO_RGB(190, 220, 250);
    return TRUE;
}

static void frame(struct Demo *d, int mode)
{
    double t = d->time, a;
    NC_U32 drawn;

    /* a lazy winding course; the map repeats, so the flight never ends */
    a = 0.35 + 0.9 * sin(0.07 * t) + 0.25 * sin(0.19 * t);
    vp->x = (float)(3000.0 + 46.0 * t + 120.0 * sin(0.11 * t));
    vp->y = (float)(3000.0 + 28.0 * t + 160.0 * cos(0.083 * t));
    vp->cosa = (float)cos(a);
    vp->sina = (float)sin(a);
    vp->altitude = (float)(262.0 + 18.0 * sin(0.31 * t));
    vp->horizon = (float)(0.36 * d->height + 0.05 * d->height * sin(0.23 * t));

    drawn = demo_run(d, mode, NCK_voxel, vp, sizeof(*vp), 0, d->width);
    sprintf(d->info, "%lu%% land", (unsigned long)(drawn / (d->width * d->height / 100)));
}

static void cleanup(struct Demo *d)
{
    nc_free(sky, d->height * 4);
    nc_free(sine, MAP_SIZE * sizeof(float));
    nc_free(colors, MAP_SIZE * MAP_SIZE * 4);
    nc_free(heights, MAP_SIZE * MAP_SIZE);
    nc_free(tp, sizeof(*tp));
    nc_free(vp, sizeof(*vp));
}

static const struct DemoApp app = {
    "voxeldemo", "flight over a 1024 x 1024 height map, one ray per screen column",
    640, 480, 1, init, frame, NULL, cleanup, NULL
};

int main(void)
{
    return demo_main(&app, &voxeldemo_kernels);
}

#include "../common/demofw.c"
