/* bench kernels: compiled for the Pi (bench.narm) and for the 68k (reference / fallback) */

#include "bench_kernels.h"

#if NC_NATIVE
#include <arm_neon.h>
#include <arm_acle.h>
#endif

#define ADLER_MOD   65521
#define ADLER_NMAX  5552

NC_KERNEL(adler32)
{
    struct SumParams *p = params;
    const NC_U8 *buf = NC_PTR(const NC_U8, p->buf);
    NC_U32 len = p->len, n;
    NC_U32 s1 = p->init & 0xffff, s2 = p->init >> 16;

    while (len)
    {
        n = len < ADLER_NMAX ? len : ADLER_NMAX;
        len -= n;
        while (n--)
        {
            s1 += *buf++;
            s2 += s1;
        }
        s1 %= ADLER_MOD;
        s2 %= ADLER_MOD;
    }
    return (s2 << 16) | s1;
}

#if NC_NATIVE
/* 16 bytes per step. For a run of k blocks starting with sums (s1, s2):
     s1' = s1 + sum(bytes)
     s2' = s2 + 16*k*s1 + 16 * sum(s1 before each block) + sum(weight * byte), weight 16..1 */
NC_KERNEL(adler32_neon)
{
    static const uint8_t weights[16] = { 16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1 };
    struct SumParams *p = params;
    const uint8_t *buf = NC_PTR(const uint8_t, p->buf);
    uint32_t len = p->len;
    uint64_t s1 = p->init & 0xffff, s2 = p->init >> 16;
    const uint8x16_t w = vld1q_u8(weights);

    while (len >= 16)
    {
        uint32_t blocks = (len < ADLER_NMAX ? len : ADLER_NMAX) / 16;
        uint32x4_t v_s1 = vdupq_n_u32(0), v_prev = vdupq_n_u32(0), v_s2 = vdupq_n_u32(0);

        len -= blocks * 16;
        for (uint32_t i = 0; i < blocks; i++)
        {
            uint8x16_t b = vld1q_u8(buf);
            buf += 16;
            v_prev = vaddq_u32(v_prev, v_s1);
            v_s1 = vpadalq_u16(v_s1, vpaddlq_u8(b));
            uint16x8_t m = vmull_u8(vget_low_u8(b), vget_low_u8(w));
            m = vmlal_u8(m, vget_high_u8(b), vget_high_u8(w));
            v_s2 = vpadalq_u16(v_s2, m);
        }
        s2 = (s2 + 16 * (uint64_t)blocks * s1 + 16 * vaddlvq_u32(v_prev) + vaddvq_u32(v_s2)) % ADLER_MOD;
        s1 = (s1 + vaddvq_u32(v_s1)) % ADLER_MOD;
    }
    while (len--)
    {
        s1 += *buf++;
        s2 += s1;
    }
    return (uint32_t)((s2 % ADLER_MOD) << 16 | (s1 % ADLER_MOD));
}
#else
NC_KERNEL(adler32_neon)
{
    return adler32(params);
}
#endif

/* CRC32 (IEEE, reflected, as zlib without the final inversion: pass ~crc, get ~crc) */
static NC_U32 crc_table[256];
static int crc_table_ready;

NC_KERNEL(crc32_table)
{
    struct SumParams *p = params;
    const NC_U8 *buf = NC_PTR(const NC_U8, p->buf);
    NC_U32 len = p->len, crc = p->init, i, c;
    int k;

    if (!crc_table_ready)
    {
        for (i = 0; i < 256; i++)
        {
            c = i;
            for (k = 0; k < 8; k++)
                c = (c & 1) ? 0xedb88320UL ^ (c >> 1) : c >> 1;
            crc_table[i] = c;
        }
        crc_table_ready = 1;
    }
    while (len--)
        crc = crc_table[(crc ^ *buf++) & 0xff] ^ (crc >> 8);
    return crc;
}

#if NC_NATIVE
/* ARMv8 CRC32 instructions take the data least significant byte first; a big-endian load
   puts the first byte in memory at the top: swap */
NC_KERNEL(crc32_hw)
{
    struct SumParams *p = params;
    const uint8_t *buf = NC_PTR(const uint8_t, p->buf);
    uint32_t len = p->len, crc = p->init;

    while (len && ((uintptr_t)buf & 7))
    {
        crc = __crc32b(crc, *buf++);
        len--;
    }
    for (; len >= 8; len -= 8, buf += 8)
        crc = __crc32d(crc, __builtin_bswap64(*(const uint64_t *)buf));
    while (len--)
        crc = __crc32b(crc, *buf++);
    return crc;
}
#else
NC_KERNEL(crc32_hw)
{
    return crc32_table(params);
}
#endif

NC_KERNEL(xform)
{
    struct XformParams *p = params;
    const float *m = NC_PTR(const float, p->matrix);
    const float *in = NC_PTR(const float, p->in);
    float *out = NC_PTR(float, p->out);
    NC_U32 i;
    int r;

    for (i = 0; i < p->count; i++, in += 4, out += 4)
        for (r = 0; r < 4; r++)
            NC_PUTF(&out[r], NC_GETF(&m[4 * r]) * NC_GETF(&in[0]) + NC_GETF(&m[4 * r + 1]) * NC_GETF(&in[1]) +
                             NC_GETF(&m[4 * r + 2]) * NC_GETF(&in[2]) + NC_GETF(&m[4 * r + 3]) * NC_GETF(&in[3]));
    return p->count;
}

#if NC_NATIVE
NC_KERNEL(xform_neon)
{
    struct XformParams *p = params;
    const float *m = NC_PTR(const float, p->matrix);
    const float *in = NC_PTR(const float, p->in);
    float *out = NC_PTR(float, p->out);
    float32x4x4_t rows = vld1q_f32_x4(m), cols;

    float32x4x2_t t01 = vtrnq_f32(rows.val[0], rows.val[1]);
    float32x4x2_t t23 = vtrnq_f32(rows.val[2], rows.val[3]);
    cols.val[0] = vcombine_f32(vget_low_f32(t01.val[0]), vget_low_f32(t23.val[0]));
    cols.val[1] = vcombine_f32(vget_low_f32(t01.val[1]), vget_low_f32(t23.val[1]));
    cols.val[2] = vcombine_f32(vget_high_f32(t01.val[0]), vget_high_f32(t23.val[0]));
    cols.val[3] = vcombine_f32(vget_high_f32(t01.val[1]), vget_high_f32(t23.val[1]));

    for (uint32_t i = 0; i < p->count; i++, in += 4, out += 4)
    {
        float32x4_t v = vld1q_f32(in);
        float32x4_t r = vmulq_laneq_f32(cols.val[0], v, 0);
        r = vfmaq_laneq_f32(r, cols.val[1], v, 1);
        r = vfmaq_laneq_f32(r, cols.val[2], v, 2);
        r = vfmaq_laneq_f32(r, cols.val[3], v, 3);
        vst1q_f32(out, r);
    }
    return p->count;
}
#else
NC_KERNEL(xform_neon)
{
    return xform(params);
}
#endif

NC_MODULE(bench, BENCH_KERNELS)
