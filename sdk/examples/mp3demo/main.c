/*
    mp3demo - plays an MP3 file through AHI while an ARM core that Emu68 leaves idle does the
    decoding: the 68k only hands finished buffers to ahi.device. An example of work that goes
    on in the background (nc_start_mp / nc_wait) while the 68k program does something else.
    The decoder is minimp3 (public domain), compiled as a kernel.

        mp3demo <file.mp3> [SECONDS=<s>] [VOLUME=<0-100>]   play (Ctrl-C stops)
        mp3demo <file.mp3> BENCH [SECONDS=<s>]      no sound: decode the first <s> seconds
                                                    (default 20) on the 68k and natively,
                                                    compare the time and the samples

    Without native calls the 68k decodes (and may not keep up). Playing needs AHI.
*/

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/io.h>
#include <devices/timer.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/timer.h>
#include <stdio.h>
#include <string.h>

#include <nativecall/nativecall.h>
#include "mp3demo_kernels.h"

#if defined(__VBCC__)
#define COMPILER "VBCC"
#else
#define COMPILER "GCC"
#endif

#define CHUNK_FRAMES    24      /* MP3 frames in one buffer for AHI: 0.63 s at 44.1 kHz */
#define BENCH_FRAMES    4       /* per call in the benchmark: a native call stays below 1 ms */
#define CHUNK_BYTES     (CHUNK_FRAMES * MP3_FRAME_MAX * 2 * 2)

/* What this program needs of devices/ahi.h (not in the NDK) */
#define AHI_NAME        "ahi.device"
#define AHI_TYPE_M16S   1       /* mono, 16 bit signed */
#define AHI_TYPE_S16S   3       /* stereo, 16 bit signed */

struct AhiRequest
{
    struct IOStdReq std;
    UWORD version;
    UWORD pad;
    ULONG private_[2];
    ULONG type;
    ULONG frequency;
    LONG volume;            /* 16.16 */
    LONG position;          /* 16.16: 0x8000 is the middle */
    struct AhiRequest *link;
};

struct Device *TimerBase;
static struct timerequest tr;

static const char *file_name;
static ULONG arg_seconds, arg_volume = 100;
static int arg_bench;

static struct NCKernels *k;
static UBYTE *file;
static ULONG file_size, file_start;
static APTR state;

static double now_s(void)
{
    struct EClockVal ev;
    ULONG freq = ReadEClock(&ev);

    return ((double)ev.ev_hi * 4294967296.0 + (double)ev.ev_lo) / freq;
}

static BOOL load_file(void)
{
    BPTR fh = Open((STRPTR)file_name, MODE_OLDFILE);
    LONG size;
    BOOL ok = FALSE;

    if (!fh)
    {
        PrintFault(IoErr(), (STRPTR)file_name);
        return FALSE;
    }
    Seek(fh, 0, OFFSET_END);
    size = Seek(fh, 0, OFFSET_BEGINNING);
    if (size > 0 && (file = nc_alloc((ULONG)size)) != NULL)
    {
        file_size = (ULONG)size;
        ok = Read(fh, file, size) == size;
    }
    if (!ok)
        printf("Cannot read %s\n", file_name);
    Close(fh);

    /* an ID3v2 tag in front of the sound: its length is four 7 bit numbers */
    if (ok && file_size > 10 && file[0] == 'I' && file[1] == 'D' && file[2] == '3')
    {
        file_start = 10 + (((ULONG)file[6] & 0x7f) << 21 | ((ULONG)file[7] & 0x7f) << 14
                           | ((ULONG)file[8] & 0x7f) << 7 | ((ULONG)file[9] & 0x7f));
        if (file_start >= file_size)
            file_start = 0;
    }
    return ok;
}

/* ---- BENCH ---- */

/* Decodes up to max_frames MP3 frames from the start of the file into out, with the 68k kernel
   or natively. Returns the samples per channel; *hz and *channels of the sound (0 if none). */
static ULONG decode_all(BOOL native, WORD *out, ULONG max_frames, struct Mp3Params *p, struct Mp3Result *r,
                        double *seconds, ULONG *hz, ULONG *channels)
{
    ULONG pos = file_start, frames = 0, samples = 0;

    *hz = *channels = 0;
    double t0 = now_s();

    p->range.from = 0;
    p->range.to = 1;
    p->in = (NC_U32)file;
    p->in_size = file_size;
    p->state = (NC_U32)state;
    p->result = (NC_U32)r;
    p->start = 1;
    while (frames < max_frames)
    {
        p->pos = pos;
        p->out = (NC_U32)out;
        p->max_frames = max_frames - frames < BENCH_FRAMES ? max_frames - frames : BENCH_FRAMES;
        if (native)
            nc_call(k, NCK_mp3_decode, p);
        else
            nc_call_68k(k, NCK_mp3_decode, p);
        if (native && nc_status(k) != NC_ST_OK)
        {
            printf("native call failed, status 0x%lx\n", (unsigned long)nc_status(k));
            break;
        }
        p->start = 0;
        /* the call that meets the end of the file decodes nothing and says 0 Hz */
        if (r->samples)
        {
            *hz = r->hz;
            *channels = r->channels;
        }
        pos += r->consumed;
        frames += r->frames;
        samples += r->samples;
        out += r->samples * r->channels;
        if (r->end || r->frames == 0)
            break;
    }
    *seconds = now_s() - t0;
    return samples;
}

static int bench(void)
{
    ULONG seconds = arg_seconds ? arg_seconds : 20;
    ULONG max_frames = seconds * 48000 / MP3_FRAME_MAX + 1, bytes = max_frames * MP3_FRAME_MAX * 2 * 2;
    struct Mp3Params *p = nc_alloc(sizeof(*p));
    struct Mp3Result *r = nc_alloc(sizeof(*r));
    WORD *out68k = nc_alloc(bytes), *outnat = nc_alloc(bytes);
    ULONG samples, n, i, differ = 0, hz, channels;
    LONG maxdiff = 0;
    double t68k, tnat, audio;
    int rc = RETURN_OK;

    if (!p || !r || !out68k || !outnat)
    {
        printf("Out of memory\n");
        rc = RETURN_FAIL;
        goto out;
    }
    printf("Decoding up to %lu s of %s on the 68k...\n", (unsigned long)seconds, file_name);
    fflush(stdout);
    samples = decode_all(FALSE, out68k, max_frames, p, r, &t68k, &hz, &channels);
    if (!samples || !hz)
    {
        printf("No MP3 sound found in the file\n");
        rc = RETURN_WARN;
        goto out;
    }
    audio = (double)samples / hz;
    printf("\nmp3demo: %.1f s of sound, %lu Hz, %s; 68k part built with %s\n\n", audio, (unsigned long)hz,
           channels == 2 ? "stereo" : "mono", COMPILER);
    printf("  68k      %8.0f ms  = %6.1f x real time\n", 1000 * t68k, audio / t68k);

    if (nc_is_native(k))
    {
        struct NCKernelStats ks;

        ULONG nhz, nch;

        n = decode_all(TRUE, outnat, max_frames, p, r, &tnat, &nhz, &nch);
        printf("  native   %8.0f ms  = %6.1f x real time  (%.1fx faster)\n", 1000 * tnat, audio / tnat, t68k / tnat);
        if (nc_stats(k, NCK_mp3_decode, &ks, FALSE) && ks.calls)
            printf("           %lu native calls, %lu us each on average, the longest %lu us\n",
                   (unsigned long)ks.calls, (unsigned long)(ks.total_us / ks.calls), (unsigned long)ks.max_us);
        if (n != samples || nhz != hz || nch != channels)
            printf("\n  DIFFERENT SOUND: 68k %lu samples at %lu Hz x %lu, native %lu at %lu Hz x %lu\n",
                   (unsigned long)samples, (unsigned long)hz, (unsigned long)channels, (unsigned long)n,
                   (unsigned long)nhz, (unsigned long)nch);
        else
        {
            for (i = 0; i < samples * channels; i++)
            {
                LONG d = (LONG)out68k[i] - (LONG)outnat[i];

                if (d < 0)
                    d = -d;
                if (d)
                    differ++;
                if (d > maxdiff)
                    maxdiff = d;
            }
            printf("\n  68k and native samples: %lu of %lu differ, by at most %ld (of 32768)\n",
                   (unsigned long)differ, (unsigned long)(samples * channels), (long)maxdiff);
        }
    }
    else
        printf("\n  no native calls on this machine\n");
    printf("\n");

out:
    nc_free(outnat, bytes);
    nc_free(out68k, bytes);
    nc_free(r, sizeof(*r));
    nc_free(p, sizeof(*p));
    return rc;
}

/* ---- play ---- */

static int play(void)
{
    struct MsgPort *port = CreateMsgPort();
    struct AhiRequest *req[2] = { NULL, NULL }, *link = NULL;
    struct Mp3Params *p[2] = { NULL, NULL };
    struct Mp3Result *r[2] = { NULL, NULL };
    WORD *buf[2] = { NULL, NULL };
    struct NCJob job;
    BOOL device = FALSE, running = FALSE, stop = FALSE, end = FALSE;
    ULONG pos = file_start, samples = 0, hz = 0, channels = 0, chunks = 0, port_sig;
    double t0 = 0;
    int cur = 0, rc = RETURN_OK, i;

    if (port)
        req[0] = (struct AhiRequest *)CreateIORequest(port, sizeof(struct AhiRequest));
    if (req[0])
    {
        req[0]->version = 4;
        device = OpenDevice((STRPTR)AHI_NAME, 0, (struct IORequest *)req[0], 0) == 0;
    }
    if (!device)
    {
        printf("Cannot open ahi.device (unit 0): playing needs AHI. \"mp3demo <file> BENCH\" works without.\n");
        rc = RETURN_WARN;
        goto out;
    }
    req[1] = AllocMem(sizeof(struct AhiRequest), MEMF_PUBLIC);
    for (i = 0; i < 2; i++)
    {
        p[i] = nc_alloc(sizeof(struct Mp3Params));
        r[i] = nc_alloc(sizeof(struct Mp3Result));
        buf[i] = nc_alloc(CHUNK_BYTES);
    }
    if (!req[1] || !p[0] || !p[1] || !r[0] || !r[1] || !buf[0] || !buf[1])
    {
        printf("Out of memory\n");
        rc = RETURN_FAIL;
        goto out;
    }
    CopyMem(req[0], req[1], sizeof(struct AhiRequest));
    port_sig = 1UL << port->mp_SigBit;

    for (i = 0; i < 2; i++)
    {
        p[i]->in = (NC_U32)file;
        p[i]->in_size = file_size;
        p[i]->out = (NC_U32)buf[i];
        p[i]->max_frames = CHUNK_FRAMES;
        p[i]->state = (NC_U32)state;
        p[i]->result = (NC_U32)r[i];
    }

    printf("Playing %s; decoding %s. Ctrl-C stops.\n", file_name,
           !nc_is_native(k) ? "on the 68k (no native calls here)"
           : nc_cores(k) > 1 ? "on an ARM core that Emu68 leaves idle" : "natively");
    fflush(stdout);

    /* Two buffers take turns: while AHI plays one, the next piece of the file is decoded into
       the other - by a job that runs beside the 68k program. */
    p[0]->pos = pos;
    p[0]->start = 1;
    nc_start_mp(k, &job, NCK_mp3_decode, p[0], sizeof(struct Mp3Params), 0, 1);
    running = TRUE;
    t0 = now_s();

    while (running)
    {
        struct AhiRequest *q = req[cur];

        nc_wait(k, &job);                   /* the buffer 'cur' is ready (or the job failed) */
        running = FALSE;
        if (nc_is_native(k) && nc_status(k) != NC_ST_OK)
        {
            printf("native call failed, status 0x%lx\n", (unsigned long)nc_status(k));
            rc = RETURN_ERROR;
            break;
        }
        pos += r[cur]->consumed;
        end = r[cur]->end || r[cur]->frames == 0;
        if (r[cur]->samples == 0)
            break;
        hz = r[cur]->hz;
        channels = r[cur]->channels;
        samples += r[cur]->samples;
        chunks++;

        q->std.io_Message.mn_Node.ln_Pri = 0;
        q->std.io_Command = CMD_WRITE;
        q->std.io_Data = buf[cur];
        q->std.io_Length = r[cur]->samples * channels * 2;
        q->std.io_Offset = 0;
        q->frequency = hz;
        q->type = channels == 2 ? AHI_TYPE_S16S : AHI_TYPE_M16S;
        q->volume = (LONG)(0x10000 * arg_volume / 100);
        q->position = 0x8000;
        q->link = link;                     /* starts exactly when the buffer before it ends */
        SendIO((struct IORequest *)q);

        /* the other buffer is free once AHI has played it */
        if (link)
        {
            while (!CheckIO((struct IORequest *)link))
                if (Wait(port_sig | SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C)
                {
                    stop = TRUE;
                    break;
                }
            if (!stop)
                WaitIO((struct IORequest *)link);
        }
        if (SetSignal(0, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C)
            stop = TRUE;
        if (arg_seconds && hz && samples / hz >= arg_seconds)
            end = TRUE;
        link = q;
        cur ^= 1;
        if (stop)
            break;
        if (!end)
        {
            p[cur]->pos = pos;
            p[cur]->start = 0;
            nc_start_mp(k, &job, NCK_mp3_decode, p[cur], sizeof(struct Mp3Params), 0, 1);
            running = TRUE;
        }
    }

    if (stop)
    {
        /* both requests may be with AHI: take them back */
        for (i = 0; i < 2; i++)
            if (req[i]->std.io_Command == CMD_WRITE && !CheckIO((struct IORequest *)req[i]))
                AbortIO((struct IORequest *)req[i]);
        for (i = 0; i < 2; i++)
            if (req[i]->std.io_Command == CMD_WRITE)
                WaitIO((struct IORequest *)req[i]);
        printf("Stopped.\n");
    }
    else if (link)
        WaitIO((struct IORequest *)link);   /* the last buffer plays to its end */

    if (hz)
    {
        struct NCKernelStats ks;
        double audio = (double)samples / hz;

        printf("\nmp3demo: %.1f s of sound in %.1f s, %lu Hz, %s, %lu buffers\n", audio, now_s() - t0,
               (unsigned long)hz, channels == 2 ? "stereo" : "mono", (unsigned long)chunks);
        if (nc_stats(k, NCK_mp3_decode, &ks, FALSE) && ks.calls)
            printf("  decoding took %.0f ms of ARM time in all (%.0f x real time): %lu calls, %lu us each\n",
                   ks.total_us / 1000.0, audio / (ks.total_us / 1e6), (unsigned long)ks.calls,
                   (unsigned long)(ks.total_us / ks.calls));
        printf("\n");
    }
    else if (rc == RETURN_OK)
    {
        printf("No MP3 sound found in the file\n");
        rc = RETURN_WARN;
    }

out:
    for (i = 0; i < 2; i++)
    {
        nc_free(buf[i], CHUNK_BYTES);
        nc_free(r[i], sizeof(struct Mp3Result));
        nc_free(p[i], sizeof(struct Mp3Params));
    }
    if (req[1])
        FreeMem(req[1], sizeof(struct AhiRequest));
    if (device)
        CloseDevice((struct IORequest *)req[0]);
    if (req[0])
        DeleteIORequest((struct IORequest *)req[0]);
    if (port)
        DeleteMsgPort(port);
    return rc;
}

#include "../common/bigstack.c"

static int body(void)
{
    int rc = RETURN_FAIL;

    if (OpenDevice((STRPTR)TIMERNAME, UNIT_MICROHZ, (struct IORequest *)&tr, 0) != 0)
        return RETURN_FAIL;
    TimerBase = tr.tr_node.io_Device;

    k = nc_open(&mp3demo_kernels);
    state = nc_alloc(MP3_STATE_SIZE);
    if (k && state && load_file())
        rc = arg_bench ? bench() : play();

    nc_free(file, file_size);
    nc_free(state, MP3_STATE_SIZE);
    nc_close(k);
    CloseDevice((struct IORequest *)&tr);
    return rc;
}

#define TEMPLATE "FILE/A,BENCH/S,SECONDS/N,VOLUME/N"
enum { ARG_FILE, ARG_BENCH, ARG_SECONDS, ARG_VOLUME, ARG_COUNT };

int main(void)
{
    static char name[256];
    LONG args[ARG_COUNT] = { 0 };
    struct RDArgs *rda = ReadArgs(TEMPLATE, args, NULL);

    if (!rda)
    {
        PrintFault(IoErr(), "mp3demo");
        return RETURN_FAIL;
    }
    strncpy(name, (char *)args[ARG_FILE], sizeof(name) - 1);
    file_name = name;
    arg_bench = args[ARG_BENCH] != 0;
    if (args[ARG_SECONDS])
        arg_seconds = *(ULONG *)args[ARG_SECONDS];
    if (args[ARG_VOLUME] && *(ULONG *)args[ARG_VOLUME] <= 100)
        arg_volume = *(ULONG *)args[ARG_VOLUME];
    FreeArgs(rda);

    /* the decoder keeps 16 KB of work data on the stack, also in its 68k version */
    return run_with_stack(body, 65536, "mp3demo");
}
