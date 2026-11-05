/*
    cgx.h - the two cybergraphics.library calls ncdemo needs, for VBCC and GCC.
    The NDK 3.2 has no CyberGraphX headers; Picasso96 provides the library on RTG systems.
    Values from the CyberGraphX 4 SDK (cybergraphx/cybergraphics.h).
*/
#ifndef NCDEMO_CGX_H
#define NCDEMO_CGX_H

#include <exec/types.h>
#include <graphics/gfx.h>
#include <graphics/rastport.h>

extern struct Library *CyberGfxBase;

#define CYBRMATTR_BPPIX         0x80000002UL
#define CYBRMATTR_PIXFMT        0x80000004UL
#define CYBRMATTR_ISCYBERGFX    0x80000007UL

#define PIXFMT_RGB16            5
#define PIXFMT_BGR16            6
#define PIXFMT_RGB16PC          7
#define PIXFMT_BGR16PC          8
#define PIXFMT_ARGB32           11
#define PIXFMT_BGRA32           12
#define PIXFMT_RGBA32           13

#define RECTFMT_RAW             5

#if defined(__VBCC__)

ULONG __cgx_GetCyberMapAttr(__reg("a6") struct Library *, __reg("a0") struct BitMap *bm,
                            __reg("d0") ULONG attr) = "\tjsr\t-96(a6)";
#define GetCyberMapAttr(bm, attr) __cgx_GetCyberMapAttr(CyberGfxBase, (bm), (attr))

ULONG __cgx_WritePixelArray(__reg("a6") struct Library *, __reg("a0") APTR src, __reg("d0") UWORD sx,
                            __reg("d1") UWORD sy, __reg("d2") UWORD mod, __reg("a1") struct RastPort *rp,
                            __reg("d3") UWORD dx, __reg("d4") UWORD dy, __reg("d5") UWORD w,
                            __reg("d6") UWORD h, __reg("d7") UBYTE fmt) = "\tjsr\t-126(a6)";
#define WritePixelArray(src, sx, sy, mod, rp, dx, dy, w, h, fmt) \
    __cgx_WritePixelArray(CyberGfxBase, (src), (sx), (sy), (mod), (rp), (dx), (dy), (w), (h), (fmt))

#else /* GCC */

#include <inline/macros.h>

#define GetCyberMapAttr(bm, attr) \
    LP2(0x60, ULONG, GetCyberMapAttr, struct BitMap *, (bm), a0, ULONG, (attr), d0, , CyberGfxBase)

#define WritePixelArray(src, sx, sy, mod, rp, dx, dy, w, h, fmt) \
    LP10(0x7e, ULONG, WritePixelArray, APTR, (src), a0, UWORD, (sx), d0, UWORD, (sy), d1, \
         UWORD, (mod), d2, struct RastPort *, (rp), a1, UWORD, (dx), d3, UWORD, (dy), d4, \
         UWORD, (w), d5, UWORD, (h), d6, UBYTE, (fmt), d7, , CyberGfxBase)

#endif

#endif
