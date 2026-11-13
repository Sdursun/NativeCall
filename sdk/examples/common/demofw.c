/* demofw.c - see demofw.h. Included at the end of each demo's main.c. */

#include <exec/types.h>
#include <exec/memory.h>
#include <devices/timer.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <graphics/displayinfo.h>
#include <graphics/modeid.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/timer.h>
#include <stdio.h>
#include <string.h>

#include "demofw.h"
#include "cgx.h"
#include "bigstack.c"

#if defined(__VBCC__)
#define DEMO_COMPILER "VBCC"
#else
#define DEMO_COMPILER "GCC"
#endif

/* pens: the screen has the 3-3-2 palette, whatever its depth */
#define DEMO_BLACK  0x00
#define DEMO_WHITE  0xff
#define DEMO_YELLOW 0xf8

struct IntuitionBase *IntuitionBase;
struct GfxBase *GfxBase;
struct Library *CyberGfxBase;
struct Device *TimerBase;
static struct timerequest demo_tr;


static const char *demo_mode_name[DEMO_MODES] = { "68k", "native", "native MP" };

struct DemoStats
{
    ULONG frames;
    double total, compute;      /* seconds */
    double fps;                 /* last second */
    ULONG win_frames;
    double win_start;
};

static double demo_now(void)
{
    struct EClockVal ev;
    ULONG freq = ReadEClock(&ev);

    return ((double)ev.ev_hi * 4294967296.0 + (double)ev.ev_lo) / freq;
}

NC_U32 demo_run(struct Demo *d, int mode, ULONG index, APTR params, ULONG params_size, ULONG from, ULONG to)
{
    if (mode == DEMO_68K)
        return nc_run_68k(d->k, index, params, from, to);
    if (mode == DEMO_NATIVE_MP)
        return nc_run_mp(d->k, index, params, params_size, from, to);
    return nc_run(d->k, index, params, from, to);
}

static void demo_palette(struct Screen *scr)
{
    static ULONG table[1 + 256 * 3 + 1];
    ULONG *t = table;
    ULONG i;

    *t++ = (256UL << 16) | 0;
    for (i = 0; i < 256; i++)
    {
        *t++ = ((i >> 5) * 255 / 7) * 0x01010101UL;
        *t++ = (((i >> 2) & 7) * 255 / 7) * 0x01010101UL;
        *t++ = ((i & 3) * 255 / 3) * 0x01010101UL;
    }
    *t = 0;
    LoadRGB32(&scr->ViewPort, table);
}

static char demo_screen_text[80];   /* what demo_screen() chose, for the summary */

/* A CyberGraphX pixel format as DFMT_*, -1 if the demos do not write it */
static LONG demo_format(ULONG pixfmt)
{
    switch (pixfmt)
    {
        case 0:              return DFMT_RGB332;    /* PIXFMT_LUT8 */
        case PIXFMT_RGB16:   return DFMT_RGB16;
        case PIXFMT_RGB16PC: return DFMT_RGB16PC;
        case PIXFMT_BGR16:   return DFMT_BGR16;
        case PIXFMT_BGR16PC: return DFMT_BGR16PC;
        case PIXFMT_ARGB32:  return DFMT_ARGB32;
        case PIXFMT_BGRA32:  return DFMT_BGRA32;
        case PIXFMT_RGBA32:  return DFMT_RGBA32;
    }
    return -1;
}

/* A screen in a format the kernels write: an RTG mode with 16 bits per pixel first (least to
   copy), then 32, then 256 colours (Picasso96 modes are "foreign"); a planar screen works too,
   slowly. The format of an RTG mode is asked before the screen is opened, so normally only one
   screen opens. */
static struct Screen *demo_screen(ULONG width, ULONG height, const char *title, ULONG *fmt)
{
    static const ULONG depths[] = { 16, 32, 24, 8, 8 };
    struct Screen *scr;
    ULONG mode_id;
    LONG f;
    int i;

    for (i = 0; i < 5; i++)
    {
        int foreign = i < 4;

        if (depths[i] > 8 && !CyberGfxBase)
            continue;
        mode_id = BestModeID(BIDTAG_NominalWidth, width, BIDTAG_NominalHeight, height,
                             BIDTAG_Depth, depths[i],
                             foreign ? BIDTAG_DIPFMustHave : TAG_IGNORE, DIPF_IS_FOREIGN, TAG_END);
        if (mode_id == INVALID_ID)
            continue;
        if (depths[i] > 8 && (!IsCyberModeID(mode_id)
                              || demo_format(GetCyberIDAttr(CYBRIDATTR_PIXFMT, mode_id)) < 0))
            continue;
        scr = OpenScreenTags(NULL, SA_DisplayID, mode_id, SA_Width, width, SA_Height, height,
                             SA_Depth, depths[i], SA_ColorMapEntries, 256, SA_Quiet, TRUE, SA_ShowTitle, FALSE,
                             SA_Type, CUSTOMSCREEN, SA_Title, (ULONG)title, TAG_END);
        if (!scr)
            continue;
        /* what the screen really is (an "8 bit" RTG screen may keep 16 bit pixels) */
        if (CyberGfxBase && GetCyberMapAttr(scr->RastPort.BitMap, CYBRMATTR_ISCYBERGFX))
            f = demo_format(GetCyberMapAttr(scr->RastPort.BitMap, CYBRMATTR_PIXFMT));
        else
            f = DFMT_RGB332;
        if (f >= 0)
        {
            static const char *names[] = { "256 colours (3-3-2 palette)", "RGB16", "RGB16PC", "BGR16",
                                           "BGR16PC", "ARGB32", "BGRA32", "RGBA32" };

            *fmt = (ULONG)f;
            sprintf(demo_screen_text, "mode 0x%08lx, %s%s", (unsigned long)mode_id, names[f],
                    foreign ? ", RTG" : ", not RTG");
            return scr;
        }
        CloseScreen(scr);
    }
    return NULL;
}

static void demo_text(struct RastPort *rp, int x, int y, int pen, const char *s)
{
    SetAPen(rp, pen);
    Move(rp, x, y);
    Text(rp, (STRPTR)s, strlen(s));
}

#define DEMO_TEMPLATE "WIDTH/N,HEIGHT/N,SECONDS/N,AUTO/N,MODE/N"
enum { DARG_WIDTH, DARG_HEIGHT, DARG_SECONDS, DARG_AUTO, DARG_MODE, DARG_COUNT };

/* What demo_main() hands to the process that runs the demo */
static const struct DemoApp *demo_app;
static const struct NCKernelSet *demo_set;
static struct { ULONG width, height, seconds, autosw, mode; } demo_args;




static int demo_body(void)
{
    static struct Demo demo;
    static struct DemoStats st[DEMO_MODES];
    const struct DemoApp *app = demo_app;
    struct Demo *d = &demo;
    struct Screen *scr = NULL;
    struct Window *win = NULL;
    struct RastPort *rp;
    ULONG seconds = demo_args.seconds, autosw = demo_args.autosw, pixbytes = 0, align = app->align ? app->align : 1;
    int mode, nmodes, quit = 0, rc = RETURN_OK, m, line_h, base, inited = 0;
    double start, last_switch, t0, t1, t2;
    char line[160];

    d->width = demo_args.width ? demo_args.width : app->width;
    d->height = demo_args.height ? demo_args.height : app->height;
    if (d->width < 160) d->width = 160;
    if (d->height < 120) d->height = 120;
    d->width = (d->width + align - 1) / align * align;

    IntuitionBase = (struct IntuitionBase *)OpenLibrary("intuition.library", 40);
    GfxBase = (struct GfxBase *)OpenLibrary("graphics.library", 40);
    if (!IntuitionBase || !GfxBase
        || OpenDevice((STRPTR)TIMERNAME, UNIT_MICROHZ, (struct IORequest *)&demo_tr, 0) != 0)
    {
        printf("Needs AmigaOS 3.1 or newer\n");
        rc = RETURN_FAIL;
        goto out;
    }
    TimerBase = demo_tr.tr_node.io_Device;
    CyberGfxBase = OpenLibrary("cybergraphics.library", 41);   /* optional: RTG */

    d->k = nc_open(demo_set);
    if (!d->k)
    {
        rc = RETURN_FAIL;
        goto out;
    }
    d->cores = nc_cores(d->k);
    nmodes = !nc_is_native(d->k) ? 1 : d->cores > 1 ? 3 : 2;
    mode = nmodes - 1;
    if (demo_args.mode >= 1 && demo_args.mode <= (ULONG)nmodes)
        mode = (int)demo_args.mode - 1;

    scr = demo_screen(d->width, d->height, app->name, &d->fmt);
    if (!scr)
    {
        printf("No screen mode for %lux%lu\n", (unsigned long)d->width, (unsigned long)d->height);
        rc = RETURN_FAIL;
        goto out;
    }
    demo_palette(scr);
    win = OpenWindowTags(NULL, WA_CustomScreen, (ULONG)scr, WA_Left, 0, WA_Top, 0,
                         WA_Width, d->width, WA_Height, d->height, WA_Backdrop, TRUE, WA_Borderless, TRUE,
                         WA_Activate, TRUE, WA_RMBTrap, TRUE,
                         WA_IDCMP, IDCMP_VANILLAKEY | IDCMP_MOUSEBUTTONS, TAG_END);
    if (!win)
    {
        printf("Cannot open window\n");
        rc = RETURN_FAIL;
        goto out;
    }
    rp = &scr->RastPort;
    SetBPen(rp, DEMO_BLACK);
    SetDrMd(rp, JAM2);
    line_h = rp->TxHeight + 2;
    base = rp->TxBaseline + 4;

    d->bpp = DEMO_BPP(d->fmt);
    pixbytes = d->width * d->height * d->bpp;
    d->pixels = nc_alloc(pixbytes);
    if (!d->pixels || !app->init(d))
    {
        printf("Out of memory\n");
        rc = RETURN_FAIL;
        inited = d->pixels != NULL;
        goto out;
    }
    inited = 1;

    start = last_switch = demo_now();
    for (m = 0; m < DEMO_MODES; m++)
        st[m].win_start = start;

    while (!quit)
    {
        struct IntuiMessage *msg;
        int next = mode;

        d->time = demo_now() - start;
        while ((msg = (struct IntuiMessage *)GetMsg(win->UserPort)))
        {
            if (msg->Class == IDCMP_VANILLAKEY)
            {
                switch (msg->Code)
                {
                    case 27: case 'q': case 'Q': quit = 1; break;
                    case 'n': case 'N': case ' ': next = (mode + 1) % nmodes; break;
                    case '1': case '2': case '3':
                        if (msg->Code - '1' < nmodes)
                            next = msg->Code - '1';
                        break;
                    default:
                        if (app->key)
                            app->key(d, msg->Code);
                        break;
                }
            }
            else if (msg->Class == IDCMP_MOUSEBUTTONS && msg->Code == SELECTDOWN)
                next = (mode + 1) % nmodes;
            ReplyMsg((struct Message *)msg);
        }
        if (seconds && d->time >= seconds)
            quit = 1;
        if (autosw && nmodes > 1 && demo_now() - last_switch >= autosw)
            next = (mode + 1) % nmodes;
        if (next != mode)
        {
            /* a fresh measurement window for the new mode */
            mode = next;
            last_switch = demo_now();
            st[mode].win_frames = 0;
            st[mode].win_start = last_switch;
        }
        m = mode;

        t0 = demo_now();
        app->frame(d, m);
        t1 = demo_now();
        if (d->bpp > 1)
            WritePixelArray(d->pixels, 0, 0, (UWORD)(d->width * d->bpp), rp, 0, 0,
                            (UWORD)d->width, (UWORD)d->height, RECTFMT_RAW);
        else
            WriteChunkyPixels(rp, 0, 0, d->width - 1, d->height - 1, d->pixels, d->width);

        if (m != DEMO_68K && nc_status(d->k) != NC_ST_OK)
        {
            printf("native call failed, status 0x%lx\n", (unsigned long)nc_status(d->k));
            rc = RETURN_ERROR;
            quit = 1;
        }

        /* overlay */
        if (m == DEMO_68K)
            sprintf(line, " 68k (Emu68 JIT, %s) ", DEMO_COMPILER);
        else if (m == DEMO_NATIVE)
            sprintf(line, " NATIVE ARM (one core) ");
        else
            sprintf(line, " NATIVE ARM on %lu cores ", (unsigned long)d->cores);
        demo_text(rp, 8, base, m ? DEMO_YELLOW : DEMO_WHITE, line);
        sprintf(line, " %5.1f fps  %6.1f ms  %s ", st[m].fps, 1000 * (t1 - t0), d->info);
        demo_text(rp, 8, base + line_h, DEMO_WHITE, line);
        if (st[DEMO_68K].frames && (st[DEMO_NATIVE].frames || st[DEMO_NATIVE_MP].frames))
        {
            char *p = line;
            double ms68k = 1000 * st[DEMO_68K].compute / st[DEMO_68K].frames;
            int i;

            p += sprintf(p, " ms per frame: 68k %.1f", ms68k);
            for (i = 1; i < DEMO_MODES; i++)
                if (st[i].frames)
                {
                    double ms = 1000 * st[i].compute / st[i].frames;

                    p += sprintf(p, "  %s %.1f (%.1fx)", demo_mode_name[i], ms, ms > 0 ? ms68k / ms : 0.0);
                }
            strcpy(p, " ");
            demo_text(rp, 8, base + 2 * line_h, DEMO_YELLOW, line);
        }
        sprintf(line, " %s%s ", nmodes > 2 ? "1-3, N, click: next mode   " : nmodes > 1 ? "1-2, N, click: next mode   "
                : "no native calls on this machine   ", app->keys ? app->keys : "");
        strcat(line, "Esc: quit ");
        demo_text(rp, 8, (int)d->height - 6, DEMO_WHITE, line);
        t2 = demo_now();

        st[m].frames++;
        st[m].compute += t1 - t0;
        st[m].total += t2 - t0;
        st[m].win_frames++;
        if (t2 - st[m].win_start >= 1.0 || (st[m].fps == 0 && t2 - st[m].win_start > 0.3))
        {
            st[m].fps = st[m].win_frames / (t2 - st[m].win_start);
            st[m].win_frames = 0;
            st[m].win_start = t2;
        }
    }

    printf("\n%s %lux%lu: %s\nscreen: %s; 68k part built with %s\n\n", app->name, (unsigned long)d->width,
           (unsigned long)d->height, app->what, demo_screen_text, DEMO_COMPILER);
    printf("  mode        frames    fps   compute ms\n");
    for (m = 0; m < DEMO_MODES; m++)
        if (st[m].frames)
            printf("  %-10s %7lu %6.1f %12.2f\n", demo_mode_name[m], (unsigned long)st[m].frames,
                   st[m].frames / st[m].total, 1000 * st[m].compute / st[m].frames);
    if (st[DEMO_68K].frames)
        for (m = 1; m < DEMO_MODES; m++)
            if (st[m].frames && st[m].compute > 0)
                printf("\n  %s computes a frame %.1fx faster than the 68k", demo_mode_name[m],
                       (st[DEMO_68K].compute / st[DEMO_68K].frames) / (st[m].compute / st[m].frames));
    if (nmodes > 2)
        printf("\n  ARM cores for native MP: %lu", (unsigned long)d->cores);
    printf("\n\n");

out:
    if (inited && app->cleanup)
        app->cleanup(d);
    if (win) CloseWindow(win);
    if (scr) CloseScreen(scr);
    nc_free(d->pixels, pixbytes);
    nc_close(d->k);
    if (TimerBase) CloseDevice((struct IORequest *)&demo_tr);
    if (CyberGfxBase) CloseLibrary(CyberGfxBase);
    if (GfxBase) CloseLibrary((struct Library *)GfxBase);
    if (IntuitionBase) CloseLibrary((struct Library *)IntuitionBase);
    return rc;
}

int demo_main(const struct DemoApp *app, const struct NCKernelSet *set)
{
    LONG args[DARG_COUNT] = { 0 };
    struct RDArgs *rda;

    rda = ReadArgs(DEMO_TEMPLATE, args, NULL);
    if (!rda)
    {
        PrintFault(IoErr(), (STRPTR)app->name);
        return RETURN_FAIL;
    }
    if (args[DARG_WIDTH])   demo_args.width = *(ULONG *)args[DARG_WIDTH];
    if (args[DARG_HEIGHT])  demo_args.height = *(ULONG *)args[DARG_HEIGHT];
    if (args[DARG_SECONDS]) demo_args.seconds = *(ULONG *)args[DARG_SECONDS];
    if (args[DARG_AUTO])    demo_args.autosw = *(ULONG *)args[DARG_AUTO];
    if (args[DARG_MODE])    demo_args.mode = *(ULONG *)args[DARG_MODE];
    FreeArgs(rda);

    demo_app = app;
    demo_set = set;
    return run_with_stack(demo_body, 65536, app->name);
}
