/* voxeldemo kernels: a flight over a height map, the same code on the 68k and natively */
#ifndef VOXELDEMO_KERNELS_H
#define VOXELDEMO_KERNELS_H

#include <nativecall/kernel.h>

#define MAP_BITS    10
#define MAP_SIZE    (1 << MAP_BITS)     /* the map is MAP_SIZE x MAP_SIZE and repeats */
#define MAP_MASK    (MAP_SIZE - 1)
#define WAVES       10
#define WATER       44                  /* heights below this are sea */

/* Builds the map: pass 0 the heights, pass 1 the colours (needs the heights of the whole map) */
struct TerrainParams
{
    struct NCRange range;   /* map rows */
    NC_U32 heights;         /* NC_U8[MAP_SIZE * MAP_SIZE] */
    NC_U32 colors;          /* NC_U32[MAP_SIZE * MAP_SIZE], 0x00RRGGBB */
    NC_U32 sine;            /* float[MAP_SIZE]: one period of a sine */
    NC_U32 pass;
};

struct VoxelParams
{
    struct NCRange range;   /* screen columns */
    NC_U32 heights, colors;
    NC_U32 out;             /* the screen buffer */
    NC_U32 sky;             /* NC_U32[height]: the sky's colour in each row, packed for the screen */
    NC_U32 width, height;
    NC_U32 fmt, bpp;
    float x, y, altitude;   /* the eye, in map units */
    float cosa, sina;       /* where it looks */
    float fov;              /* half the view's width at distance 1 */
    float horizon;          /* screen row of the horizon */
    float scale;            /* height on screen of one unit at distance 1, in pixels */
    float distance;         /* how far the eye sees */
    NC_U32 haze;            /* 0x00RRGGBB: what the land fades into */
};

NC_CHECK_SIZE(struct VoxelParams, 80)

/* terrain: fills the rows of the range (returns 0)
   voxel:   draws the columns of the range, returns the pixels of land drawn */
#define VOXELDEMO_KERNELS(K)    K(terrain) K(voxel)

NC_DECLARE(voxeldemo, VOXELDEMO_KERNELS)

#endif
