/*
    ncdemo - a lit, rotating torus drawn in software, switchable between the 68k (Emu68 JIT)
    and native ARM while it runs. Example for the nativecall SDK.

        ncdemo [WIDTH=<w>] [HEIGHT=<h>] [SEG=<n>] [SECONDS=<s>] [AUTO=<s>] [START68K]

    N, Space or a mouse click go to the next mode (68k, native, native on all ARM cores),
    Esc or Q quits.
    SECONDS quits after that long, AUTO switches by itself every <s> seconds (for unattended
    runs); a summary goes to the Shell at the end.

    The 68k side opens the screen, builds the mesh, and copies each frame to the display
    (WriteChunkyPixels). Projection, lighting and rasterizing are kernels (ncdemo_kernels.c).
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
#include "ncdemo_kernels.h"
#include "cgx.h"

#if defined(__VBCC__)
#define COMPILER "VBCC"
#else
#define COMPILER "GCC"
#endif

#define PI 3.14159265358979

/* palette */
#define COL_BG      0
#define RAMP_A      1       /* 1..126 */
#define RAMP_B      128     /* 128..253 */
#define COL_YELLOW  254
#define COL_WHITE   255

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
    double total, transform, raster, display;   /* seconds */
    double fps;                                 /* last second */
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

        if (i == COL_BG)                         { r = 10;  g = 12;  b = 32; }
        else if (i >= RAMP_A && i < RAMP_A + RAMP_LEVELS)
        {
            int l = (i - RAMP_A) * 255 / (RAMP_LEVELS - 1);     /* deep blue -> cyan -> white */
            r = l > 170 ? (l - 170) * 3 : 0;
            g = l * 9 / 10;
            b = 60 + l * 195 / 255;
        }
        else if (i >= RAMP_B && i < RAMP_B + RAMP_LEVELS)
        {
            int l = (i - RAMP_B) * 255 / (RAMP_LEVELS - 1);     /* dark red -> orange -> yellow */
            r = 70 + l * 185 / 255;
            g = l * 8 / 10;
            b = l > 200 ? (l - 200) * 3 : 0;
        }
        else if (i == COL_YELLOW)                { r = 255; g = 220; b = 60; }
        else if (i == COL_WHITE)                 { r = 255; g = 255; b = 255; }
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

/* Palette as the screen stores it, for the present kernel. 0 if the format is not handled. */
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

/* Torus R = 1, r = 0.42, nu x nv quads, checkerboard of 4x4 quads in two colour ramps */
static void build_torus(struct Vertex *v, struct Triangle *t, int nu, int nv)
{
    int i, j, n = 0;

    for (i = 0; i < nu; i++)
        for (j = 0; j < nv; j++)
        {
            double u = 2 * PI * i / nu, w = 2 * PI * j / nv;
            double cu = cos(u), su = sin(u), cw = cos(w), sw = sin(w);

            v[i * nv + j].x = (float)((1.0 + 0.42 * cw) * cu);
            v[i * nv + j].y = (float)((1.0 + 0.42 * cw) * su);
            v[i * nv + j].z = (float)(0.42 * sw);
            v[i * nv + j].nx = (float)(cw * cu);
            v[i * nv + j].ny = (float)(cw * su);
            v[i * nv + j].nz = (float)sw;
        }
    for (i = 0; i < nu; i++)
        for (j = 0; j < nv; j++)
        {
            NC_U16 p00 = i * nv + j, p10 = ((i + 1) % nu) * nv + j;
            NC_U16 p11 = ((i + 1) % nu) * nv + (j + 1) % nv, p01 = i * nv + (j + 1) % nv;
            NC_U16 col = ((i / 4 + j / 4) & 1) ? RAMP_B : RAMP_A;

            t[n].a = p00; t[n].b = p10; t[n].c = p11; t[n].color = col; n++;
            t[n].a = p00; t[n].b = p11; t[n].c = p01; t[n].color = col; n++;
        }
}

/* rotation about x, y and z by ax, ay, az */
static void set_rotation(float *r, double ax, double ay, double az)
{
    double ca = cos(ax), sa = sin(ax), cb = cos(ay), sb = sin(ay), cc = cos(az), sc = sin(az);

    r[0] = (float)(cb * cc);                 r[1] = (float)(-cb * sc);                r[2] = (float)sb;
    r[3] = (float)(sa * sb * cc + ca * sc);  r[4] = (float)(-sa * sb * sc + ca * cc); r[5] = (float)(-sa * cb);
    r[6] = (float)(-ca * sb * cc + sa * sc); r[7] = (float)(ca * sb * sc + sa * cc);  r[8] = (float)(ca * cb);
}

static void text(struct RastPort *rp, int x, int y, int pen, const char *s)
{
    SetAPen(rp, pen);
    Move(rp, x, y);
    Text(rp, (STRPTR)s, strlen(s));
}

#define TEMPLATE "WIDTH/N,HEIGHT/N,SEG/N,SECONDS/N,AUTO/N,START68K/S"
enum { ARG_WIDTH, ARG_HEIGHT, ARG_SEG, ARG_SECONDS, ARG_AUTO, ARG_START68K, ARG_COUNT };

int main(void)
{
    LONG args[ARG_COUNT] = { 0 };
    struct RDArgs *rda;
    ULONG width = 640, height = 480, seg = 64, seconds = 0, autosw = 0;
    ULONG nverts, ntris, mode_id;
    struct NCKernels *k = NULL;
    struct Screen *scr = NULL;
    struct Window *win = NULL;
    struct Vertex *verts = NULL;
    struct SVertex *sverts = NULL;
    struct Triangle *tris = NULL;
    struct Camera *cam = NULL;
    struct TransformParams *tp = NULL;
    struct RasterParams *rp = NULL;
    struct PresentParams *pp = NULL;
    APTR lut = NULL, pixels = NULL;
    ULONG bpp = 0;
    int line_h, base;
    UBYTE *chunky = NULL;
    float *zbuf = NULL;
    struct ModeStats st[3];          /* 0: 68k, 1: native, 2: native on all cores */
    int native = 0, nmodes = 1, quit = 0, rc = RETURN_OK, m;
    double t0, t1, t2, t3, start, last_switch;
    char line[120];

    memset(st, 0, sizeof(st));
    rda = ReadArgs(TEMPLATE, args, NULL);
    if (!rda)
    {
        PrintFault(IoErr(), "ncdemo");
        return RETURN_FAIL;
    }
    if (args[ARG_WIDTH])   width = *(ULONG *)args[ARG_WIDTH];
    if (args[ARG_HEIGHT])  height = *(ULONG *)args[ARG_HEIGHT];
    if (args[ARG_SEG])     seg = *(ULONG *)args[ARG_SEG];
    if (args[ARG_SECONDS]) seconds = *(ULONG *)args[ARG_SECONDS];
    if (args[ARG_AUTO])    autosw = *(ULONG *)args[ARG_AUTO];
    if (seg < 8) seg = 8;
    if (seg > 180) seg = 180;
    nverts = seg * (seg / 2);
    ntris = 2 * nverts;

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

    k = nc_open(&ncdemo_kernels);
    if (!k)
    {
        rc = RETURN_FAIL;
        goto out;
    }
    nmodes = !nc_is_native(k) ? 1 : nc_cores(k) > 1 ? 3 : 2;
    native = args[ARG_START68K] ? 0 : nmodes - 1;

    verts = nc_alloc(nverts * sizeof(*verts));
    sverts = nc_alloc(nverts * sizeof(*sverts));
    tris = nc_alloc(ntris * sizeof(*tris));
    cam = nc_alloc(sizeof(*cam));
    tp = nc_alloc(sizeof(*tp));
    rp = nc_alloc(sizeof(*rp));
    chunky = nc_alloc(width * height);
    zbuf = nc_alloc(width * height * sizeof(float));
    if (!verts || !sverts || !tris || !cam || !tp || !rp || !chunky || !zbuf)
    {
        printf("Out of memory\n");
        rc = RETURN_FAIL;
        goto out;
    }
    build_torus(verts, tris, seg, seg / 2);

    /* RTG first (Picasso96 modes are "foreign"): on a planar AGA screen WriteChunkyPixels has
       to convert to bitplanes in chip RAM, which costs ~400 ms per frame on a PiStorm */
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
                         SA_Title, (ULONG)"ncdemo", TAG_END);
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
    SetBPen(&scr->RastPort, COL_BG);
    SetDrMd(&scr->RastPort, JAM2);
    line_h = scr->RastPort.TxHeight + 2;
    base = scr->RastPort.TxBaseline + 4;

    /* RTG screen in a format we know: convert to it ourselves (present kernel) and copy the
       pixels with WritePixelArray. Otherwise the OS converts in WriteChunkyPixels. */
    if (CyberGfxBase && GetCyberMapAttr(scr->RastPort.BitMap, CYBRMATTR_ISCYBERGFX))
    {
        ULONG fmt = GetCyberMapAttr(scr->RastPort.BitMap, CYBRMATTR_PIXFMT);

        lut = nc_alloc(256 * 4);
        pp = nc_alloc(sizeof(*pp));
        if (lut && pp && (bpp = build_lut(lut, fmt)) != 0)
            pixels = nc_alloc(width * height * bpp);
        if (!pixels)
            bpp = 0;
    }
    if (pp && bpp)
    {
        pp->chunky = (NC_U32)chunky;
        pp->out = (NC_U32)pixels;
        pp->lut = (NC_U32)lut;
        pp->width = width;
        pp->bpp = bpp;
    }

    /* camera, light, kernel parameters */
    cam->trans[0] = 0.0f; cam->trans[1] = 0.0f; cam->trans[2] = 3.4f;
    cam->focal = height * 1.15f;
    cam->cx = width * 0.5f;
    cam->cy = height * 0.5f + 8;
    {
        double lx = 0.5, ly = -0.6, lz = 1.0, n = sqrt(lx * lx + ly * ly + lz * lz);
        cam->light[0] = (float)(lx / n); cam->light[1] = (float)(ly / n); cam->light[2] = (float)(lz / n);
    }
    tp->vertices = (NC_U32)verts;
    tp->svertices = (NC_U32)sverts;
    tp->camera = (NC_U32)cam;
    rp->svertices = (NC_U32)sverts;
    rp->triangles = (NC_U32)tris;
    rp->ntriangles = ntris;
    rp->chunky = (NC_U32)chunky;
    rp->zbuffer = (NC_U32)zbuf;
    rp->width = width;
    rp->height = height;
    rp->background = COL_BG;

    start = last_switch = now_s();
    st[0].win_start = st[1].win_start = st[2].win_start = start;

    while (!quit)
    {
        struct IntuiMessage *msg;
        double t = now_s() - start;

        while ((msg = (struct IntuiMessage *)GetMsg(win->UserPort)))
        {
            if (msg->Class == IDCMP_VANILLAKEY && (msg->Code == 27 || msg->Code == 'q' || msg->Code == 'Q'))
                quit = 1;
            else if ((msg->Class == IDCMP_VANILLAKEY && (msg->Code == 'n' || msg->Code == 'N' || msg->Code == ' '))
                     || (msg->Class == IDCMP_MOUSEBUTTONS && msg->Code == SELECTDOWN))
            {
                if (nmodes > 1)
                {
                    native = (native + 1) % nmodes;
                    last_switch = now_s();
                    st[native].win_frames = 0;      /* a fresh measurement window for the new mode */
                    st[native].win_start = last_switch;
                }
            }
            ReplyMsg((struct Message *)msg);
        }
        if (seconds && t >= seconds)
            quit = 1;
        if (autosw && nmodes > 1 && now_s() - last_switch >= autosw)
        {
            native = (native + 1) % nmodes;
            last_switch = now_s();
            st[native].win_frames = 0;
            st[native].win_start = last_switch;
        }

        m = native;
        set_rotation(cam->rot, t * 0.9, t * 0.6, t * 0.25);

        /* The three kernels work on vertices or screen rows that do not depend on each other and
           do not write their params: mode 2 gives them to all ARM cores (nc_run_mp) */
        t0 = now_s();
        if (m == 2)
            nc_run_mp(k, NCK_transform, tp, sizeof(*tp), 0, nverts);
        else if (m)
            nc_run(k, NCK_transform, tp, 0, nverts);
        else
            nc_run_68k(k, NCK_transform, tp, 0, nverts);
        t1 = now_s();
        if (m == 2)
            nc_run_mp(k, NCK_raster, rp, sizeof(*rp), 0, height);
        else if (m)
            nc_run(k, NCK_raster, rp, 0, height);
        else
            nc_run_68k(k, NCK_raster, rp, 0, height);
        t2 = now_s();
        if (bpp)
        {
            if (m == 2)
                nc_run_mp(k, NCK_present, pp, sizeof(*pp), 0, height);
            else if (m)
                nc_run(k, NCK_present, pp, 0, height);
            else
                nc_run_68k(k, NCK_present, pp, 0, height);
            WritePixelArray(pixels, 0, 0, (UWORD)(width * bpp), &scr->RastPort, 0, 0,
                            (UWORD)width, (UWORD)height, RECTFMT_RAW);
        }
        else
            WriteChunkyPixels(&scr->RastPort, 0, 0, width - 1, height - 1, chunky, width);

        if (m && nc_status(k) != NC_ST_OK)
        {
            printf("native call failed, status 0x%lx\n", (unsigned long)nc_status(k));
            rc = RETURN_ERROR;
            quit = 1;
        }

        /* overlay */
        if (m == 2)
            sprintf(line, " NATIVE ARM on %lu cores ", (unsigned long)nc_cores(k));
        else
            sprintf(line, " %s ", m ? "NATIVE ARM (Cortex-A72)" : "68k (Emu68 JIT, " COMPILER ")");
        text(&scr->RastPort, 8, base, m ? COL_YELLOW : COL_WHITE, line);
        sprintf(line, " %5.1f fps  %ux%u, %lu triangles ", st[m].fps, (unsigned)width, (unsigned)height,
                (unsigned long)ntris);
        text(&scr->RastPort, 8, base + line_h, COL_WHITE, line);
        if (st[0].fps > 0 && (st[1].fps > 0 || st[2].fps > 0))
        {
            char *p = line;

            p += sprintf(p, " 68k %.1f fps", st[0].fps);
            if (st[1].fps > 0)
                p += sprintf(p, "  native %.1f (%.1fx)", st[1].fps, st[1].fps / st[0].fps);
            if (st[2].fps > 0)
                p += sprintf(p, "  native MP %.1f (%.1fx)", st[2].fps, st[2].fps / st[0].fps);
            strcpy(p, " ");
            text(&scr->RastPort, 8, base + 2 * line_h, COL_YELLOW, line);
        }
        text(&scr->RastPort, 8, height - 6, COL_WHITE,
             nmodes > 1 ? " N / click: next mode (68k, native, native on all cores)    Esc: quit "
                        : " no native calls on this machine    Esc: quit ");
        t3 = now_s();

        st[m].frames++;
        st[m].transform += t1 - t0;
        st[m].raster += t2 - t1;
        st[m].display += t3 - t2;
        st[m].total += t3 - t0;
        st[m].win_frames++;
        if (t3 - st[m].win_start >= 1.0)
        {
            st[m].fps = st[m].win_frames / (t3 - st[m].win_start);
            st[m].win_frames = 0;
            st[m].win_start = t3;
        }
    }

    printf("\nncdemo %lux%lu, %lu triangles, 68k part built with %s\n\n", (unsigned long)width,
           (unsigned long)height, (unsigned long)ntris, COMPILER);
    printf("  mode       frames    fps   transform    raster   display  (ms per frame)\n");
    for (m = 0; m < 3; m++)
        if (st[m].frames)
            printf("  %-9s %7lu %6.1f %10.2f %9.2f %9.2f\n", m == 2 ? "native MP" : m ? "native" : "68k",
                   (unsigned long)st[m].frames,
                   st[m].frames / st[m].total, 1000 * st[m].transform / st[m].frames,
                   1000 * st[m].raster / st[m].frames, 1000 * st[m].display / st[m].frames);
    if (st[0].frames && st[1].frames)
        printf("\n  native is %.1fx faster\n", (st[1].frames / st[1].total) / (st[0].frames / st[0].total));
    if (st[0].frames && st[2].frames)
        printf("%s  native MP (%lu ARM cores) is %.1fx faster\n", st[1].frames ? "" : "\n",
               (unsigned long)nc_cores(k), (st[2].frames / st[2].total) / (st[0].frames / st[0].total));
    {
        static const char *names[] = { "transform", "raster", "present" };
        struct NCKernelStats ks;
        ULONG i;

        for (i = 0; i < 3; i++)
            if (nc_stats(k, i, &ks, FALSE) && ks.calls)
            {
                if (i == 0)
                    printf("\n  native time per kernel call (measured on the ARM side):\n");
                printf("    %-10s %7lu calls  avg %5lu us  max %5lu us\n", names[i], (unsigned long)ks.calls,
                       (unsigned long)(ks.total_us / ks.calls), (unsigned long)ks.max_us);
            }
    }
    printf("\n");

out:
    if (win) CloseWindow(win);
    if (scr) CloseScreen(scr);
    nc_free(pixels, width * height * bpp);
    nc_free(pp, sizeof(*pp));
    nc_free(lut, 256 * 4);
    nc_free(zbuf, width * height * sizeof(float));
    nc_free(chunky, width * height);
    nc_free(rp, sizeof(*rp));
    nc_free(tp, sizeof(*tp));
    nc_free(cam, sizeof(*cam));
    nc_free(tris, ntris * sizeof(*tris));
    nc_free(sverts, nverts * sizeof(*sverts));
    nc_free(verts, nverts * sizeof(*verts));
    nc_close(k);
    if (TimerBase) CloseDevice((struct IORequest *)&tr);
    if (CyberGfxBase) CloseLibrary(CyberGfxBase);
    if (GfxBase) CloseLibrary((struct Library *)GfxBase);
    if (IntuitionBase) CloseLibrary((struct Library *)IntuitionBase);
    FreeArgs(rda);
    return rc;
}
