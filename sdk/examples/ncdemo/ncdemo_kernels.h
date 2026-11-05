/* ncdemo kernels: a software 3D renderer, the same code on the 68k and natively */
#ifndef NCDEMO_KERNELS_H
#define NCDEMO_KERNELS_H

#include <nativecall/kernel.h>

/* Mesh vertex (object space) and projected vertex (screen space) */
struct Vertex
{
    float x, y, z;
    float nx, ny, nz;
};

struct SVertex
{
    float x, y;         /* pixels */
    float z;            /* camera space depth, > 0 */
    float shade;        /* 0..1 */
};

struct Triangle
{
    NC_U16 a, b, c;
    NC_U16 color;       /* first palette index of the triangle's colour ramp */
};

/* Camera: rotation (row major), translation, projection, light direction (camera space,
   pointing from the light into the scene, normalized) */
struct Camera
{
    float rot[9];
    float trans[3];
    float focal, cx, cy;
    float light[3];
};

struct TransformParams
{
    struct NCRange range;   /* vertices */
    NC_U32 vertices;        /* struct Vertex * */
    NC_U32 svertices;       /* struct SVertex * */
    NC_U32 camera;          /* struct Camera * */
};

struct RasterParams
{
    struct NCRange range;   /* screen rows */
    NC_U32 svertices;
    NC_U32 triangles;       /* struct Triangle * */
    NC_U32 ntriangles;
    NC_U32 chunky;          /* UBYTE[width * height] */
    NC_U32 zbuffer;         /* float[width * height] */
    NC_U32 width, height;
    NC_U32 background;      /* palette index */
};

struct PresentParams
{
    struct NCRange range;   /* screen rows */
    NC_U32 chunky;          /* UBYTE[width * height] palette indexes */
    NC_U32 out;             /* width * height pixels in the screen's format */
    NC_U32 lut;             /* 256 pixels in the screen's format (2 or 4 bytes each) */
    NC_U32 width;
    NC_U32 bpp;             /* 2 or 4 */
};

#define RAMP_LEVELS 126     /* palette entries per colour ramp */

/* transform: project vertices, light them (returns 0)
   raster:    clear and draw the rows of the range (returns pixels drawn)
   present:   palette indexes -> screen pixel format (RTG 16/32 bit) */
#define NCDEMO_KERNELS(K) K(transform) K(raster) K(present)

NC_DECLARE(ncdemo, NCDEMO_KERNELS)

#endif
