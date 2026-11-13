/*
    filterdemo - picture processing: a picture is turned and scaled with bilinear filtering, then
    a 3 x 3 filter runs over the result (blur, sharpen, edges, emboss). Two kernels, one after
    the other; both work row by row, so the ARM cores share the rows. Example for the nativecall
    SDK.

        filterdemo [WIDTH=<w>] [HEIGHT=<h>] [SECONDS=<s>] [AUTO=<s>] [MODE=<1-3>]

    F chooses the next filter (otherwise it changes every five seconds). Other keys and
    arguments: ../common/demofw.h.
*/

#include <math.h>
#include <stdio.h>

#include "../common/demofw.h"
#include "filterdemo_kernels.h"

static struct RotoParams *rp;
static struct FilterParams *fp;
static NC_U32 *source, *work;
static ULONG work_bytes;
static int chosen = -1;         /* filter picked with F, -1: by the clock */

#define FILTERS 5
static const struct
{
    const char *name;
    NC_S32 weight[9], shift, bias;
} filters[FILTERS] = {
    { "no filter", {  0,  0,  0,   0,  1,  0,   0,  0,  0 }, 0, 0 },
    { "blur",      {  1,  2,  1,   2,  4,  2,   1,  2,  1 }, 4, 0 },
    { "sharpen",   {  0, -1,  0,  -1,  5, -1,   0, -1,  0 }, 0, 0 },
    { "edges",     { -1, -1, -1,  -1,  8, -1,  -1, -1, -1 }, 0, 0 },
    { "emboss",    { -2, -1,  0,  -1,  0,  1,   0,  1,  2 }, 0, 128 },
};

/* The picture: coloured tiles with a light disc on each and dark joints - plenty of edges */
static void make_picture(void)
{
    static const ULONG tints[8] = { 0xd84030, 0x30a848, 0x3060d8, 0xe0b020, 0xb040c0, 0x20b0b8, 0xe07828, 0x8890a0 };
    ULONG x, y;

    for (y = 0; y < SRC_SIZE; y++)
        for (x = 0; x < SRC_SIZE; x++)
        {
            ULONG lx = x & 63, ly = y & 63, tile = (x >> 6) * 3 + (y >> 6) * 5;
            ULONG c = tints[tile & 7], r = c >> 16, g = (c >> 8) & 0xff, b = c & 0xff;
            LONG dx = (LONG)lx - 32, dy = (LONG)ly - 32;
            ULONG shade = 150 + ly + (lx >> 1);         /* 150..244 of 256 */

            if (lx < 3 || ly < 3)
                r = g = b = 28;                         /* joint */
            else if (dx * dx + dy * dy < 17 * 17)
            {
                r = (r + 255 * 2) / 3;                  /* disc */
                g = (g + 255 * 2) / 3;
                b = (b + 255 * 2) / 3;
            }
            source[y * SRC_SIZE + x] = DEMO_RGB(r * shade >> 8, g * shade >> 8, b * shade >> 8);
        }
}

static BOOL init(struct Demo *d)
{
    work_bytes = d->width * d->height * 4;
    rp = nc_alloc(sizeof(*rp));
    fp = nc_alloc(sizeof(*fp));
    source = nc_alloc(SRC_SIZE * SRC_SIZE * 4);
    work = nc_alloc(work_bytes);
    if (!rp || !fp || !source || !work)
        return FALSE;
    make_picture();

    rp->src = (NC_U32)source;
    rp->work = (NC_U32)work;
    rp->width = d->width;
    rp->height = d->height;
    fp->work = (NC_U32)work;
    fp->out = (NC_U32)d->pixels;
    fp->width = d->width;
    fp->height = d->height;
    fp->fmt = d->fmt;
    fp->bpp = d->bpp;
    return TRUE;
}

static void frame(struct Demo *d, int mode)
{
    double t = d->time, angle = 0.25 * t, zoom = 1.0 + 0.75 * sin(0.4 * t);     /* 0.25 .. 1.75 */
    double c = cos(angle) * zoom * 65536.0, s = sin(angle) * zoom * 65536.0;
    double cx = 0.5 * d->width, cy = 0.5 * d->height;
    int which = chosen >= 0 ? chosen : (int)(t / 5.0) % FILTERS, i;

    /* the picture turns around the middle of the screen and drifts */
    rp->dudx = (NC_S32)c;
    rp->dvdx = (NC_S32)s;
    rp->dudy = (NC_S32)-s;
    rp->dvdy = (NC_S32)c;
    rp->u0 = (NC_S32)(fmod(256.0 + 40.0 * t, (double)SRC_SIZE) * 65536.0 - cx * c + cy * s);
    rp->v0 = (NC_S32)(fmod(256.0 + 25.0 * t, (double)SRC_SIZE) * 65536.0 - cx * s - cy * c);
    demo_run(d, mode, NCK_rotozoom, rp, sizeof(*rp), 0, d->height);

    for (i = 0; i < 9; i++)
        fp->weight[i] = filters[which].weight[i];
    fp->shift = filters[which].shift;
    fp->bias = filters[which].bias;
    demo_run(d, mode, NCK_filter, fp, sizeof(*fp), 0, d->height);
    sprintf(d->info, "%s", filters[which].name);
}

static void key(struct Demo *d, int code)
{
    if (code == 'f' || code == 'F')
        chosen = ((chosen >= 0 ? chosen : (int)(d->time / 5.0) % FILTERS) + 1) % FILTERS;
}

static void cleanup(struct Demo *d)
{
    (void)d;
    nc_free(work, work_bytes);
    nc_free(source, SRC_SIZE * SRC_SIZE * 4);
    nc_free(fp, sizeof(*fp));
    nc_free(rp, sizeof(*rp));
}

static const struct DemoApp app = {
    "filterdemo", "picture turned and scaled (bilinear), then a 3 x 3 filter",
    640, 480, 1, init, frame, key, cleanup, "F: filter   "
};

int main(void)
{
    return demo_main(&app, &filterdemo_kernels);
}

#include "../common/demofw.c"
