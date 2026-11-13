/*
    nbodydemo - two small galaxies of 1500 bodies in all fall into each other. Every body feels
    the gravity of every other one: 2.25 million pairs for each step. A step for one body only
    reads the old state and writes its own entry of the new one, so the ARM cores share the
    bodies. Example for the nativecall SDK.

        nbodydemo [WIDTH=<w>] [HEIGHT=<h>] [SECONDS=<s>] [AUTO=<s>] [MODE=<1-3>]

    R starts again. Other keys and arguments: ../common/demofw.h. Each frame is one time step:
    a faster mode shows the same story sooner.
*/

#include <math.h>
#include <stdio.h>

#include "../common/demofw.h"
#include "nbodydemo_kernels.h"

#define BODIES  1500
#define PI      3.14159265358979

static struct StepParams *sp;
static struct DrawParams *dp;
static struct Body *bodies[2];
static int current;
static ULONG steps;
static ULONG seed;

static double rnd(void)             /* 0..1 */
{
    seed = seed * 1103515245UL + 12345UL;
    return ((seed >> 8) & 0xffff) / 65536.0;
}

/* A disc of `count` light bodies around a heavy one, turning as gravity asks */
static void galaxy(struct Body *b, int count, double cx, double cy, double cz, double vx, double vz,
                   double core_mass, double radius, double lean, ULONG tint)
{
    int i;

    b[0].x = (float)cx; b[0].y = (float)cy; b[0].z = (float)cz;
    b[0].vx = (float)vx; b[0].vy = 0.0f; b[0].vz = (float)vz;
    b[0].mass = (float)core_mass;
    b[0].color = 0xfff0c0;
    for (i = 1; i < count; i++)
    {
        double r = radius * (0.2 + 0.8 * sqrt(rnd())), a = 2 * PI * rnd();
        double speed = sqrt(core_mass / r);     /* a circular orbit */
        double x = r * cos(a), z = r * sin(a), y = 0.04 * radius * (rnd() - 0.5);
        double px = -speed * sin(a), pz = speed * cos(a);
        ULONG k = (ULONG)(150 + 105 * rnd());

        /* the disc leans around the x axis */
        b[i].x = (float)(cx + x);
        b[i].y = (float)(cy + y * cos(lean) - z * sin(lean));
        b[i].z = (float)(cz + y * sin(lean) + z * cos(lean));
        b[i].vx = (float)(vx + px);
        b[i].vy = (float)(-pz * sin(lean));
        b[i].vz = (float)(vz + pz * cos(lean));
        b[i].mass = (float)(0.02 + 0.06 * rnd());
        b[i].color = tint == 0 ? DEMO_RGB(k * 150 / 255, k * 190 / 255, k)      /* bluish */
                               : DEMO_RGB(k, k * 180 / 255, k * 120 / 255);     /* reddish */
    }
}

static void restart(void)
{
    seed = 12345;
    galaxy(bodies[0], 900, -2.6, 0.0, 0.0, 0.0, 0.22, 420.0, 1.9, 0.35, 0);
    galaxy(bodies[0] + 900, BODIES - 900, 3.4, 0.4, 0.0, 0.0, -0.33, 280.0, 1.5, -0.9, 1);
    current = 0;
    steps = 0;
}

static BOOL init(struct Demo *d)
{
    sp = nc_alloc(sizeof(*sp));
    dp = nc_alloc(sizeof(*dp));
    bodies[0] = nc_alloc(BODIES * sizeof(struct Body));
    bodies[1] = nc_alloc(BODIES * sizeof(struct Body));
    if (!sp || !dp || !bodies[0] || !bodies[1])
        return FALSE;
    restart();

    sp->count = BODIES;
    sp->dt = 0.001f;
    sp->soft = 0.0025f;
    dp->count = BODIES;
    dp->out = (NC_U32)d->pixels;
    dp->width = d->width;
    dp->height = d->height;
    dp->fmt = d->fmt;
    dp->bpp = d->bpp;
    dp->scale = (float)d->height / 11.0f;
    dp->tilt_cos = (float)cos(0.55);
    dp->tilt_sin = (float)sin(0.55);
    return TRUE;
}

static void frame(struct Demo *d, int mode)
{
    sp->from = (NC_U32)bodies[current];
    sp->to = (NC_U32)bodies[current ^ 1];
    demo_run(d, mode, NCK_step, sp, sizeof(*sp), 0, BODIES);
    current ^= 1;
    steps++;

    dp->bodies = (NC_U32)bodies[current];
    dp->cosa = (float)cos(0.1 * d->time);
    dp->sina = (float)sin(0.1 * d->time);
    demo_run(d, mode, NCK_draw, dp, sizeof(*dp), 0, d->height);
    sprintf(d->info, "step %lu, %d bodies", (unsigned long)steps, BODIES);
    if (steps >= 6000)
        restart();                  /* the story is over: the galaxies have merged */
}

static void key(struct Demo *d, int code)
{
    (void)d;
    if (code == 'r' || code == 'R')
        restart();
}

static void cleanup(struct Demo *d)
{
    (void)d;
    nc_free(bodies[1], BODIES * sizeof(struct Body));
    nc_free(bodies[0], BODIES * sizeof(struct Body));
    nc_free(dp, sizeof(*dp));
    nc_free(sp, sizeof(*sp));
}

static const struct DemoApp app = {
    "nbodydemo", "gravity between 1500 bodies, all pairs, one step per frame",
    640, 480, 4, init, frame, key, cleanup, "R: restart   "
};

int main(void)
{
    return demo_main(&app, &nbodydemo_kernels);
}

#include "../common/demofw.c"
