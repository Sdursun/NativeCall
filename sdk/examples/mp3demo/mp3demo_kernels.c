/* mp3demo kernel: compiled for the Pi (mp3demo.narm) and for the 68k (fallback) */

#include "mp3demo_kernels.h"

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
/* plain C on both sides: minimp3's NEON code gives wrong samples on big-endian AArch64 */
#define MINIMP3_NO_SIMD
#include "minimp3.h"

typedef char mp3_state_fits[sizeof(mp3dec_t) <= MP3_STATE_SIZE ? 1 : -1];

NC_KERNEL(mp3_decode)
{
    const struct Mp3Params *p = params;
    struct Mp3Result *r = NC_PTR(struct Mp3Result, p->result);
    mp3dec_t *dec = NC_PTR(mp3dec_t, p->state);
    const uint8_t *in = NC_PTR(const uint8_t, p->in);
    mp3d_sample_t *out = NC_PTR(mp3d_sample_t, p->out);
    NC_U32 pos = p->pos, samples = 0, frames = 0, hz = 0, channels = 0, end = 0;

    if (p->range.from >= p->range.to)
        return 0;
    if (p->start)
        mp3dec_init(dec);

    while (frames < p->max_frames)
    {
        mp3dec_frame_info_t info;
        int n;

        if (pos >= p->in_size)
        {
            end = 1;
            break;
        }
        n = mp3dec_decode_frame(dec, in + pos, (int)(p->in_size - pos), out, &info);
        if (info.frame_bytes == 0)
        {
            end = 1;            /* no frame in what is left of the file */
            break;
        }
        pos += (NC_U32)info.frame_bytes;
        if (n > 0)
        {
            out += n * info.channels;
            samples += (NC_U32)n;
            hz = (NC_U32)info.hz;
            channels = (NC_U32)info.channels;
            frames++;
        }
        /* n == 0: a tag or damaged data was skipped */
    }
    r->consumed = pos - p->pos;
    r->samples = samples;
    r->hz = hz;
    r->channels = channels;
    r->frames = frames;
    r->end = end;
    return samples;
}

NC_MODULE(mp3demo, MP3DEMO_KERNELS)
