/*
    demofw.h - what the graphical demos share: a screen, the modes 68k / native / native on all
    cores, switching between them while the demo runs, the measurements and the summary.

    A demo provides a struct DemoApp and calls demo_main(); its main.c ends with
    #include "../common/demofw.c" (one compilation unit, so the SDK's Makefile needs nothing
    extra).

        <demo> [WIDTH=<w>] [HEIGHT=<h>] [SECONDS=<s>] [AUTO=<s>] [MODE=<1-3>]

    1-3 choose 68k, native, native on all cores; N, Space or a mouse click go to the next mode;
    Esc or Q quits. SECONDS quits after that long, AUTO goes to the next mode every <s> seconds
    (for unattended runs); a summary goes to the Shell at the end.
*/
#ifndef DEMOFW_H
#define DEMOFW_H

#include <exec/types.h>
#include <nativecall/nativecall.h>
#include "demopix.h"

#define DEMO_68K        0
#define DEMO_NATIVE     1
#define DEMO_NATIVE_MP  2
#define DEMO_MODES      3

struct Demo
{
    ULONG width, height;
    APTR pixels;                /* width * height * bpp bytes in the screen's format (nc_alloc) */
    ULONG bpp, fmt;             /* DFMT_* (demopix.h) */
    struct NCKernels *k;
    ULONG cores;                /* ARM cores of DEMO_NATIVE_MP */
    double time;                /* seconds since the start: the moment the frame shows */
    char info[100];             /* a line of its own on the screen, set by frame() (may stay empty) */
};

struct DemoApp
{
    const char *name;
    const char *what;                           /* one line for the summary */
    ULONG width, height;                        /* default size */
    ULONG align;                                /* the width becomes a multiple of this */
    BOOL (*init)(struct Demo *d);               /* allocate (nc_alloc), FALSE: out of memory */
    void (*frame)(struct Demo *d, int mode);    /* draw the frame for d->time into d->pixels */
    void (*key)(struct Demo *d, int code);      /* other keys (may be NULL) */
    void (*cleanup)(struct Demo *d);
    const char *keys;                           /* help for those keys, e.g. "F: filter   " */
};

int demo_main(const struct DemoApp *app, const struct NCKernelSet *set);

/* nc_run_68k / nc_run / nc_run_mp, by mode */
NC_U32 demo_run(struct Demo *d, int mode, ULONG index, APTR params, ULONG params_size, ULONG from, ULONG to);

#endif
