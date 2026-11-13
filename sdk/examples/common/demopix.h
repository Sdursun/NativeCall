/*
    demopix.h - pixels of the demo screen, for kernels (68k and native) and for demofw.c.

    The demos compute colours as 0x00RRGGBB and store them the way the screen keeps them, so
    that showing a frame is one copy (WritePixelArray) with no conversion on the 68k.
*/
#ifndef DEMOPIX_H
#define DEMOPIX_H

#include <nativecall/kernel.h>

/* How the screen stores a pixel (struct Demo.fmt) */
#define DFMT_RGB332     0   /* 1 byte: index into a 3-3-2 palette (screens with 256 colours) */
#define DFMT_RGB16      1   /* 2 bytes: 5-6-5, big-endian */
#define DFMT_RGB16PC    2   /* 2 bytes: 5-6-5, little-endian */
#define DFMT_BGR16      3
#define DFMT_BGR16PC    4
#define DFMT_ARGB32     5   /* 4 bytes */
#define DFMT_BGRA32     6
#define DFMT_RGBA32     7

#define DEMO_BPP(fmt)   ((fmt) == DFMT_RGB332 ? 1 : (fmt) <= DFMT_BGR16PC ? 2 : 4)

#define DEMO_RGB(r, g, b)   (((NC_U32)(r) << 16) | ((NC_U32)(g) << 8) | (NC_U32)(b))

/* One colour (0x00RRGGBB) as the screen stores it: the value of a byte, a 16 bit or a 32 bit
   word, by DEMO_BPP(fmt). For code that writes single pixels (demo_put). */
static inline NC_U32 demo_pack(NC_U32 c, NC_U32 fmt)
{
    NC_U32 v;

    switch (fmt)
    {
        case DFMT_RGB332:
            return ((c >> 16) & 0xe0) | ((c >> 11) & 0x1c) | ((c >> 6) & 0x03);
        case DFMT_RGB16:
            return ((c >> 8) & 0xf800) | ((c >> 5) & 0x07e0) | ((c >> 3) & 0x001f);
        case DFMT_RGB16PC:
            v = ((c >> 8) & 0xf800) | ((c >> 5) & 0x07e0) | ((c >> 3) & 0x001f);
            return ((v << 8) | (v >> 8)) & 0xffff;
        case DFMT_BGR16:
            return ((c << 8) & 0xf800) | ((c >> 5) & 0x07e0) | ((c >> 19) & 0x001f);
        case DFMT_BGR16PC:
            v = ((c << 8) & 0xf800) | ((c >> 5) & 0x07e0) | ((c >> 19) & 0x001f);
            return ((v << 8) | (v >> 8)) & 0xffff;
        case DFMT_ARGB32:
            return 0xff000000UL | c;
        case DFMT_BGRA32:
            return ((c & 0xff) << 24) | ((c & 0xff00) << 8) | ((c >> 8) & 0xff00) | 0xff;
    }
    return (c << 8) | 0xff;     /* DFMT_RGBA32 */
}

/* Stores a packed pixel; bpp is 1, 2 or 4 */
static inline void demo_put(void *dst, NC_U32 packed, NC_U32 bpp)
{
    if (bpp == 2)
        *(NC_U16 *)dst = (NC_U16)packed;
    else if (bpp == 4)
        *(NC_U32 *)dst = packed;
    else
        *(NC_U8 *)dst = (NC_U8)packed;
}

/* n pixels (0x00RRGGBB each) to their place in the screen buffer */
static inline void demo_store(void *dst, const NC_U32 *src, NC_U32 n, NC_U32 fmt)
{
    NC_U8 *d8 = dst;
    NC_U16 *d16 = dst;
    NC_U32 *d32 = dst;
    NC_U32 i, c, v;

    switch (fmt)
    {
        case DFMT_RGB332:
            for (i = 0; i < n; i++)
            {
                c = src[i];
                d8[i] = (NC_U8)(((c >> 16) & 0xe0) | ((c >> 11) & 0x1c) | ((c >> 6) & 0x03));
            }
            break;
        case DFMT_RGB16:
            for (i = 0; i < n; i++)
            {
                c = src[i];
                d16[i] = (NC_U16)(((c >> 8) & 0xf800) | ((c >> 5) & 0x07e0) | ((c >> 3) & 0x001f));
            }
            break;
        case DFMT_RGB16PC:
            for (i = 0; i < n; i++)
            {
                c = src[i];
                v = ((c >> 8) & 0xf800) | ((c >> 5) & 0x07e0) | ((c >> 3) & 0x001f);
                d16[i] = (NC_U16)((v << 8) | (v >> 8));
            }
            break;
        case DFMT_BGR16:
            for (i = 0; i < n; i++)
            {
                c = src[i];
                d16[i] = (NC_U16)(((c << 8) & 0xf800) | ((c >> 5) & 0x07e0) | ((c >> 19) & 0x001f));
            }
            break;
        case DFMT_BGR16PC:
            for (i = 0; i < n; i++)
            {
                c = src[i];
                v = ((c << 8) & 0xf800) | ((c >> 5) & 0x07e0) | ((c >> 19) & 0x001f);
                d16[i] = (NC_U16)((v << 8) | (v >> 8));
            }
            break;
        case DFMT_ARGB32:
            for (i = 0; i < n; i++)
                d32[i] = 0xff000000UL | src[i];
            break;
        case DFMT_BGRA32:
            for (i = 0; i < n; i++)
            {
                c = src[i];
                d32[i] = ((c & 0xff) << 24) | ((c & 0xff00) << 8) | ((c >> 8) & 0xff00) | 0xff;
            }
            break;
        default:    /* DFMT_RGBA32 */
            for (i = 0; i < n; i++)
                d32[i] = (src[i] << 8) | 0xff;
            break;
    }
}

#endif
