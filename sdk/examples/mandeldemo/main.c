/*
    mandeldemo - a Mandelbrot zoom into Seahorse Valley, switchable while it runs between
    the 68k (Emu68 JIT, FPU), native ARM (double), native NEON (float, 4 pixels at a time) and
    NEON on all free ARM cores (nc_run_mp, hypervisor 3). Example for the nativecall SDK.

        mandeldemo [WIDTH=<w>] [HEIGHT=<h>] [ZOOM=<s>] [SECONDS=<s>] [AUTO=<s>] [MODE=<1-4>]

    1-4 choose 68k, native, NEON, NEON on all cores; N, Space or a mouse click go to the next
    mode; C turns colour cycling on or off; Esc or Q quits.
    ZOOM: seconds for one zoom in (and as long back out), default 30.
    SECONDS quits after that long, AUTO goes to the next mode every <s> seconds (for unattended
    runs); a summary goes to the Shell at the end.

    The view depends only on the time, not on the frame: every mode shows the same picture
    at the same moment and differs only in how many frames it manages.
*/

#include <exec/types.h>
#include <exec/memory.h>
#include <devices/timer.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <graphics/displayinfo.h>
#include <graphics/modeid.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/timer.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include <nativecall/nativecall.h>
#include "mandeldemo_kernels.h"
#include "cgx.h"

#if defined(__VBCC__)
#define COMPILER "VBCC"
#else
#define COMPILER "GCC"
#endif

#define PI 3.14159265358979

/* zoom target (Seahorse Valley) and the view widths at both ends of the zoom */
#define CX          (-0.743643887037151)
#define CY          (0.131825904205330)
#define WIDTH_OUT   3.2
#define WIDTH_IN    0.0002      /* deeper gets blocky in single precision (NEON) */

/* palette: 0 inside, 1..253 ramp, then the text pens */
#define COL_BLACK   0
#define COL_WHITE   254
#define COL_YELLOW  255

#define MODES 4
static const char *mode_name[MODES] = { "68k", "native", "NEON", "NEON MP" };
static const char *mode_title[MODES] = {
    "68k (Emu68 JIT, FPU, double, " COMPILER ")",
    "NATIVE ARM (Cortex-A72, double)",
    "NATIVE NEON (float, 4 pixels at once)",
    "NATIVE NEON on %lu ARM cores"
};

struct IntuitionBase *IntuitionBase;
struct GfxBase *GfxBase;
struct Library *CyberGfxBase;
struct Device *TimerBase;
static struct timerequest tr;

static double now_s(void)
{
    struct EClockVal ev;
    ULONG freq = ReadEClock(&ev);
    return ((double)ev.ev_hi * 4294967296.0 + (double)ev.ev_lo) / freq;
}

struct ModeStats
{
    ULONG frames;
    double total, compute;      /* seconds */
    double iters;               /* iterations computed */
    double fps;                 /* last second */
    ULONG win_frames;
    double win_start;
};

static UBYTE pal[256][3];

static void set_palette(struct Screen *scr)
{
    static ULONG table[1 + 256 * 3 + 1];
    ULONG *t = table;
    int i;

    *t++ = (256UL << 16) | 0;
    for (i = 0; i < 256; i++)
    {
        ULONG r = 0, g = 0, b = 0;

        if (i >= 1 && i <= MD_COLORS)
        {
            /* smooth cycle: dark blue -> white -> orange -> dark blue */
            double a = 2 * PI * (i - 1) / MD_COLORS;

            r = (ULONG)(127.5 + 127.5 * sin(a - 1.9));
            g = (ULONG)(127.5 + 120.0 * sin(a - 1.2));
            b = (ULONG)(127.5 + 127.5 * sin(a + 0.1));
        }
        else if (i == COL_WHITE)    { r = 255; g = 255; b = 255; }
        else if (i == COL_YELLOW)   { r = 255; g = 220; b = 60; }
        if (r > 255) r = 255;
        if (g > 255) g = 255;
        if (b > 255) b = 255;
        pal[i][0] = (UBYTE)r;
        pal[i][1] = (UBYTE)g;
        pal[i][2] = (UBYTE)b;
        *t++ = r * 0x01010101UL;
        *t++ = g * 0x01010101UL;
        *t++ = b * 0x01010101UL;
    }
    *t = 0;
    LoadRGB32(&scr->ViewPort, table);
}

/* Palette as the screen stores it, for the kernels. 0 if the format is not handled. */
static ULONG build_lut(APTR lut, ULONG fmt)
{
    UWORD *l16 = lut;
    ULONG *l32 = lut;
    int i;

    for (i = 0; i < 256; i++)
    {
        ULONG r = pal[i][0], g = pal[i][1], b = pal[i][2];
        UWORD rgb = (UWORD)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
        UWORD bgr = (UWORD)(((b >> 3) << 11) | ((g >> 2) << 5) | (r >> 3));

        switch (fmt)
        {
            case PIXFMT_RGB16:   l16[i] = rgb; break;
            case PIXFMT_RGB16PC: l16[i] = (UWORD)((rgb << 8) | (rgb >> 8)); break;
            case PIXFMT_BGR16:   l16[i] = bgr; break;
            case PIXFMT_BGR16PC: l16[i] = (UWORD)((bgr << 8) | (bgr >> 8)); break;
            case PIXFMT_ARGB32:  l32[i] = 0xff000000UL | (r << 16) | (g << 8) | b; break;
            case PIXFMT_BGRA32:  l32[i] = (b << 24) | (g << 16) | (r << 8) | 0xff; break;
            case PIXFMT_RGBA32:  l32[i] = (r << 24) | (g << 16) | (b << 8) | 0xff; break;
            default: return 0;
        }
    }
    return (fmt >= PIXFMT_ARGB32) ? 4 : 2;
}

static void text(struct RastPort *rp, int x, int y, int pen, const char *s)
{
    SetAPen(rp, pen);
    Move(rp, x, y);
    Text(rp, (STRPTR)s, strlen(s));
}

#define TEMPLATE "WIDTH/N,HEIGHT/N,ZOOM/N,SECONDS/N,AUTO/N,MODE/N"
enum { ARG_WIDTH, ARG_HEIGHT, ARG_ZOOM, ARG_SECONDS, ARG_AUTO, ARG_MODE, ARG_COUNT };

int main(void)
{
    LONG args[ARG_COUNT] = { 0 };
    struct RDArgs *rda;
    ULONG width = 640, height = 480, zoom = 30, seconds = 0, autosw = 0, mode_id, pieces;
    struct NCKernels *k = NULL;
    struct Screen *scr = NULL;
    struct Window *win = NULL;
    struct MandelParams *mp = NULL;
    APTR lut = NULL, pixels = NULL;
    ULONG bpp = 1, pixbytes = 0;
    int line_h, base;
    struct ModeStats st[MODES];
    int mode = 0, quit = 0, cycle = 1, rc = RETURN_OK, m, nmodes;
    double t0, t1, t2, start, last_switch;
    char line[120];

    memset(st, 0, sizeof(st));
    rda = ReadArgs(TEMPLATE, args, NULL);
    if (!rda)
    {
        PrintFault(IoErr(), "mandeldemo");
        return RETURN_FAIL;
    }
    if (args[ARG_WIDTH])   width = *(ULONG *)args[ARG_WIDTH];
    if (args[ARG_HEIGHT])  height = *(ULONG *)args[ARG_HEIGHT];
    if (args[ARG_ZOOM])    zoom = *(ULONG *)args[ARG_ZOOM];
    if (args[ARG_SECONDS]) seconds = *(ULONG *)args[ARG_SECONDS];
    if (args[ARG_AUTO])    autosw = *(ULONG *)args[ARG_AUTO];
    width = (width + 4 * MD_SEGS - 1) / (4 * MD_SEGS) * (4 * MD_SEGS);
    if (width < 64) width = 64;
    if (height < 48) height = 48;
    if (zoom < 2) zoom = 2;
    pieces = height * MD_SEGS;

    IntuitionBase = (struct IntuitionBase *)OpenLibrary("intuition.library", 40);
    GfxBase = (struct GfxBase *)OpenLibrary("graphics.library", 40);
    if (!IntuitionBase || !GfxBase || OpenDevice((STRPTR)TIMERNAME, UNIT_MICROHZ, (struct IORequest *)&tr, 0) != 0)
    {
        printf("Needs AmigaOS 3.1 or newer\n");
        rc = RETURN_FAIL;
        goto out;
    }
    TimerBase = tr.tr_node.io_Device;
    CyberGfxBase = OpenLibrary("cybergraphics.library", 41);   /* optional: RTG fast path */

    k = nc_open(&mandeldemo_kernels);
    if (!k)
    {
        rc = RETURN_FAIL;
        goto out;
    }
    nmodes = !nc_is_native(k) ? 1 : nc_cores(k) > 1 ? 4 : 3;
    mode = nmodes - 1;
    if (args[ARG_MODE] && *(ULONG *)args[ARG_MODE] >= 1 && *(ULONG *)args[ARG_MODE] <= (ULONG)nmodes)
        mode = *(ULONG *)args[ARG_MODE] - 1;

    mp = nc_alloc(sizeof(*mp));
    if (!mp)
    {
        printf("Out of memory\n");
        rc = RETURN_FAIL;
        goto out;
    }

    /* RTG first (Picasso96 modes are "foreign"): planar AGA would need a chunky to planar
       conversion in chip RAM for every frame */
    mode_id = BestModeID(BIDTAG_NominalWidth, width, BIDTAG_NominalHeight, height, BIDTAG_Depth, 8,
                         BIDTAG_DIPFMustHave, DIPF_IS_FOREIGN, TAG_END);
    if (mode_id == INVALID_ID)
        mode_id = BestModeID(BIDTAG_NominalWidth, width, BIDTAG_NominalHeight, height, BIDTAG_Depth, 8, TAG_END);
    if (mode_id == INVALID_ID)
    {
        printf("No %lux%lu 256 colour screen mode\n", (unsigned long)width, (unsigned long)height);
        rc = RETURN_FAIL;
        goto out;
    }
    scr = OpenScreenTags(NULL, SA_DisplayID, mode_id, SA_Width, width, SA_Height, height, SA_Depth, 8,
                         SA_Quiet, TRUE, SA_ShowTitle, FALSE, SA_Type, CUSTOMSCREEN,
                         SA_Title, (ULONG)"mandeldemo", TAG_END);
    if (!scr)
    {
        printf("Cannot open screen\n");
        rc = RETURN_FAIL;
        goto out;
    }
    set_palette(scr);
    win = OpenWindowTags(NULL, WA_CustomScreen, (ULONG)scr, WA_Left, 0, WA_Top, 0,
                         WA_Width, width, WA_Height, height, WA_Backdrop, TRUE, WA_Borderless, TRUE,
                         WA_Activate, TRUE, WA_RMBTrap, TRUE,
                         WA_IDCMP, IDCMP_VANILLAKEY | IDCMP_MOUSEBUTTONS, TAG_END);
    if (!win)
    {
        printf("Cannot open window\n");
        rc = RETURN_FAIL;
        goto out;
    }
    SetBPen(&scr->RastPort, COL_BLACK);
    SetDrMd(&scr->RastPort, JAM2);
    line_h = scr->RastPort.TxHeight + 2;
    base = scr->RastPort.TxBaseline + 4;

    /* RTG screen in a format we know: the kernels write its pixels directly and
       WritePixelArray copies them. Otherwise palette indexes and WriteChunkyPixels. */
    if (CyberGfxBase && GetCyberMapAttr(scr->RastPort.BitMap, CYBRMATTR_ISCYBERGFX))
    {
        ULONG fmt = GetCyberMapAttr(scr->RastPort.BitMap, CYBRMATTR_PIXFMT);

        lut = nc_alloc(256 * 4);
        if (lut && (bpp = build_lut(lut, fmt)) == 0)
            bpp = 1;
    }
    pixbytes = width * height * bpp;
    pixels = nc_alloc(pixbytes);
    if (!pixels)
    {
        printf("Out of memory\n");
        rc = RETURN_FAIL;
        goto out;
    }
    mp->width = width;
    mp->height = height;
    mp->out = (NC_U32)pixels;
    mp->lut = (NC_U32)lut;
    mp->bpp = bpp;

    start = last_switch = now_s();
    for (m = 0; m < MODES; m++)
        st[m].win_start = start;

    while (!quit)
    {
        struct IntuiMessage *msg;
        double t = now_s() - start, phase, view_w;
        int next = mode;
        NC_U32 iters;

        while ((msg = (struct IntuiMessage *)GetMsg(win->UserPort)))
        {
            if (msg->Class == IDCMP_VANILLAKEY)
            {
                switch (msg->Code)
                {
                    case 27: case 'q': case 'Q': quit = 1; break;
                    case 'n': case 'N': case ' ': next = (mode + 1) % nmodes; break;
                    case '1': case '2': case '3': case '4':
                        if (msg->Code - '1' < nmodes)
                            next = msg->Code - '1';
                        break;
                    case 'c': case 'C': cycle = !cycle; break;
                }
            }
            else if (msg->Class == IDCMP_MOUSEBUTTONS && msg->Code == SELECTDOWN)
                next = (mode + 1) % nmodes;
            ReplyMsg((struct Message *)msg);
        }
        if (seconds && t >= seconds)
            quit = 1;
        if (autosw && nmodes > 1 && now_s() - last_switch >= autosw)
            next = (mode + 1) % nmodes;
        if (next != mode)
        {
            /* start a fresh measurement window for the new mode */
            mode = next;
            last_switch = now_s();
            st[mode].win_frames = 0;
            st[mode].win_start = last_switch;
        }
        m = mode;

        /* zoom in and back out, exponentially (constant speed to the eye) */
        phase = fmod(t / zoom, 2.0);
        if (phase > 1.0)
            phase = 2.0 - phase;
        phase = 0.5 - 0.5 * cos(PI * phase);           /* ease at both ends */
        view_w = WIDTH_OUT * pow(WIDTH_IN / WIDTH_OUT, phase);
        mp->step = view_w / width;
        mp->x0 = CX - 0.5 * width * mp->step;
        mp->y0 = CY - 0.5 * height * mp->step;
        mp->maxiter = 64 + (ULONG)(22.0 * log(WIDTH_OUT / view_w) / log(2.0));
        mp->shift = cycle ? (ULONG)(t * 40.0) % MD_COLORS : 0;

        t0 = now_s();
        if (m == 0)
            iters = nc_run_68k(k, NCK_mandel, mp, 0, pieces);
        else if (m == 3)
            iters = nc_run_mp(k, NCK_mandel_neon, mp, sizeof(*mp), 0, pieces);
        else
            iters = nc_run(k, m == 1 ? NCK_mandel : NCK_mandel_neon, mp, 0, pieces);
        t1 = now_s();
        if (bpp > 1)
            WritePixelArray(pixels, 0, 0, (UWORD)(width * bpp), &scr->RastPort, 0, 0,
                            (UWORD)width, (UWORD)height, RECTFMT_RAW);
        else
            WriteChunkyPixels(&scr->RastPort, 0, 0, width - 1, height - 1, pixels, width);

        if (m && nc_status(k) != NC_ST_OK)
        {
            printf("native call failed, status 0x%lx\n", (unsigned long)nc_status(k));
            rc = RETURN_ERROR;
            quit = 1;
        }

        /* overlay */
        line[0] = ' ';
        sprintf(line + 1, mode_title[m], (unsigned long)nc_cores(k));
        strcat(line, " ");
        text(&scr->RastPort, 8, base, m ? COL_YELLOW : COL_WHITE, line);
        sprintf(line, " %5.1f fps  %4.0f ms  zoom %.0fx  %lu iterations max ", st[m].fps,
                1000 * (t1 - t0), WIDTH_OUT / view_w, (unsigned long)mp->maxiter);
        text(&scr->RastPort, 8, base + line_h, COL_WHITE, line);
        /* the modes ran at different depths: compare iterations per second, not fps */
        if (nmodes > 1 && st[0].compute > 0)
        {
            char *p = line;
            int i;

            p += sprintf(p, " Miter/s: ");
            for (i = 0; i < MODES; i++)
                if (st[i].compute > 0)
                    p += sprintf(p, "%s %.0f  ", mode_name[i], st[i].iters / st[i].compute / 1e6);
            for (i = MODES - 1; i > 0; i--)
                if (st[i].compute > 0)
                {
                    sprintf(p, "= %.1fx ", (st[i].iters / st[i].compute) / (st[0].iters / st[0].compute));
                    break;
                }
            text(&scr->RastPort, 8, base + 2 * line_h, COL_YELLOW, line);
        }
        text(&scr->RastPort, 8, height - 6, COL_WHITE,
             nmodes > 3 ? " 1-4, N, click: next mode   C: colours   Esc: quit "
             : nmodes > 1 ? " 1-3, N, click: next mode   C: colours   Esc: quit "
                        : " no native calls on this machine    Esc: quit ");
        t2 = now_s();

        st[m].frames++;
        st[m].compute += t1 - t0;
        st[m].total += t2 - t0;
        st[m].iters += iters;
        st[m].win_frames++;
        if (t2 - st[m].win_start >= 1.0 || (st[m].fps == 0 && st[m].win_frames >= 1 && t2 - st[m].win_start > 0.3))
        {
            st[m].fps = st[m].win_frames / (t2 - st[m].win_start);
            st[m].win_frames = 0;
            st[m].win_start = t2;
        }
    }

    printf("\nmandeldemo %lux%lu, zoom into Seahorse Valley, 68k part built with %s\n\n",
           (unsigned long)width, (unsigned long)height, COMPILER);
    printf("  mode     frames    fps   ms/frame   Miter/s\n");
    for (m = 0; m < MODES; m++)
        if (st[m].frames)
            printf("  %-7s %7lu %6.1f %10.1f %9.1f\n", mode_name[m], (unsigned long)st[m].frames,
                   st[m].frames / st[m].total, 1000 * st[m].compute / st[m].frames,
                   st[m].iters / st[m].compute / 1e6);
    if (st[0].frames)
        for (m = 1; m < MODES; m++)
            if (st[m].frames)
                printf("\n  %s computes %.1fx faster than the 68k", mode_name[m],
                       (st[m].iters / st[m].compute) / (st[0].iters / st[0].compute));
    printf("\n");
    {
        struct NCKernelStats ks;
        ULONG i;
        int header = 0;

        for (i = 0; i < 2; i++)
            if (nc_stats(k, i, &ks, FALSE) && ks.calls)
            {
                if (!header++)
                    printf("\n  native time per kernel call (measured on the ARM side):\n");
                printf("    %-12s %7lu calls  avg %5lu us  max %5lu us\n", i ? "mandel_neon*" : "mandel",
                       (unsigned long)ks.calls, (unsigned long)(ks.total_us / ks.calls), (unsigned long)ks.max_us);
            }
    }
    if (nmodes > 3)
        printf("\n  * NEON and NEON MP together; ARM cores for NEON MP: %lu\n", (unsigned long)nc_cores(k));
    printf("\n");

out:
    if (win) CloseWindow(win);
    if (scr) CloseScreen(scr);
    nc_free(pixels, pixbytes);
    nc_free(lut, 256 * 4);
    nc_free(mp, sizeof(*mp));
    nc_close(k);
    if (TimerBase) CloseDevice((struct IORequest *)&tr);
    if (CyberGfxBase) CloseLibrary(CyberGfxBase);
    if (GfxBase) CloseLibrary((struct Library *)GfxBase);
    if (IntuitionBase) CloseLibrary((struct Library *)IntuitionBase);
    FreeArgs(rda);
    return rc;
}
