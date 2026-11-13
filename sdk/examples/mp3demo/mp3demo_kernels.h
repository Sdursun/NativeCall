/* mp3demo kernel: an MP3 decoder (minimp3, public domain), the same code on the 68k and natively */
#ifndef MP3DEMO_KERNELS_H
#define MP3DEMO_KERNELS_H

#include <nativecall/kernel.h>

#define MP3_STATE_SIZE  8192        /* room for the decoder's state between calls (mp3dec_t) */
#define MP3_FRAME_MAX   1152        /* samples per channel in one MP3 frame */

/* Written by the kernel: what one call did */
struct Mp3Result
{
    NC_U32 consumed;        /* bytes of the file used */
    NC_U32 samples;         /* samples per channel decoded */
    NC_U32 hz, channels;    /* of the last frame decoded */
    NC_U32 frames;          /* MP3 frames decoded */
    NC_U32 end;             /* 1: nothing more to decode in the file */
};

/* The kernel does not write these: a job started with nc_start_mp() gets a copy */
struct Mp3Params
{
    struct NCRange range;   /* one piece: { 0, 1 } */
    NC_U32 in, in_size;     /* the whole MP3 file */
    NC_U32 pos;             /* where to go on in it */
    NC_U32 out;             /* 16 bit samples, big-endian, interleaved: room for
                               max_frames * MP3_FRAME_MAX * 2 of them */
    NC_U32 max_frames;      /* decode at most this many MP3 frames */
    NC_U32 state;           /* MP3_STATE_SIZE bytes, kept between calls */
    NC_U32 result;          /* struct Mp3Result * */
    NC_U32 start;           /* 1: a new file, forget the decoder's state */
};

NC_CHECK_SIZE(struct Mp3Params, 40)
NC_CHECK_SIZE(struct Mp3Result, 24)

/* mp3_decode: returns the samples per channel it decoded */
#define MP3DEMO_KERNELS(K)  K(mp3_decode)

NC_DECLARE(mp3demo, MP3DEMO_KERNELS)

#endif
