/*
    nativecall/nc_math.h - small math for kernels, the same results on the 68k and natively.

    There is no libm in native modules. These are plain C (or one ARM instruction where it gives
    the same result), so a kernel and its 68k fallback compute alike.

        nc_fabsf  nc_floorf  nc_ceilf  nc_truncf  nc_roundf  nc_fmodf  nc_sqrtf
        nc_minf   nc_maxf    nc_clampf
        NC_MIN    NC_MAX     NC_CLAMP   (any type, arguments evaluated more than once)

    nc_fmodf is x - trunc(x / y) * y: exact enough for angles and texture coordinates, not
    bit-identical to libm's fmodf for huge x / y ratios.
*/

#ifndef NATIVECALL_NC_MATH_H
#define NATIVECALL_NC_MATH_H

#include <nativecall/kernel.h>

#define NC_MIN(a, b)            ((a) < (b) ? (a) : (b))
#define NC_MAX(a, b)            ((a) > (b) ? (a) : (b))
#define NC_CLAMP(x, lo, hi)     ((x) < (lo) ? (lo) : (x) > (hi) ? (hi) : (x))

/* above this every float is an integer */
#define NC__FLOAT_INT_LIMIT     8388608.0f

static inline float nc_fabsf(float x)
{
    return x < 0.0f ? -x : x;
}

static inline float nc_truncf(float x)
{
    if (nc_fabsf(x) >= NC__FLOAT_INT_LIMIT)
        return x;
    return (float)(long)x;
}

static inline float nc_floorf(float x)
{
    float t = nc_truncf(x);

    return (t > x) ? t - 1.0f : t;
}

static inline float nc_ceilf(float x)
{
    float t = nc_truncf(x);

    return (t < x) ? t + 1.0f : t;
}

/* halfway cases away from zero, like C99 roundf */
static inline float nc_roundf(float x)
{
    return x < 0.0f ? -nc_floorf(-x + 0.5f) : nc_floorf(x + 0.5f);
}

static inline float nc_fmodf(float x, float y)
{
    return x - nc_truncf(x / y) * y;
}

/* IEEE square root: correctly rounded on both sides (ARM fsqrt, 68k FPU fsqrt) */
#if NC_NATIVE
static inline float nc_sqrtf(float x)
{
    float r;

    __asm__("fsqrt %s0, %s1" : "=w"(r) : "w"(x));
    return r;
}
#elif NC_X64
/* sqrtss: no libm in the x64 modules either */
static inline float nc_sqrtf(float x)
{
    return __builtin_sqrtf(x);      /* -fno-math-errno (mk): one instruction */
}
#else
#include <math.h>
static inline float nc_sqrtf(float x)
{
    return (float)sqrt((double)x);
}
#endif

/*
    nc_sin, nc_cos, nc_exp (double) and nc_sinf, nc_cosf, nc_expf: plain C, the same code on
    both sides (no libm natively). Range reduction then a series: about 1e-9 relative to libm
    for |x| up to ~1e5 (sin/cos) and over the whole double range (exp). Not bit-identical to
    libm; identical between a kernel and its 68k version as long as the native side is built
    with -ffp-contract=off (otherwise ARM fuses multiply-adds).
*/
#define NC_PI       3.14159265358979323846
#define NC_TWO_PI   6.28318530717958647693
#define NC_HALF_PI  1.57079632679489661923
#define NC_LN2      0.69314718055994530942

/* 32-bit conversion where it fits: on the 68k a 64-bit one is a slow library call (GCC) */
static inline double nc__round(double x)
{
    if (x > -2.0e9 && x < 2.0e9)
        return x < 0.0 ? -(double)(long)(-x + 0.5) : (double)(long)(x + 0.5);
    return x < 0.0 ? -(double)(long long)(-x + 0.5) : (double)(long long)(x + 0.5);
}

static inline double nc_sin(double x)
{
    /* x - k*2pi in two steps (the 2pi split keeps the low bits), then into [-pi/2, pi/2] */
    double k = nc__round(x * (1.0 / NC_TWO_PI));
    double r = (x - k * 6.28318530717958623200) - k * 2.44929359829470635445e-16;
    double r2, s;

    if (r > NC_HALF_PI)
        r = NC_PI - r;
    else if (r < -NC_HALF_PI)
        r = -NC_PI - r;
    r2 = r * r;
    /* Taylor to r^15: below 1e-12 on [-pi/2, pi/2] */
    s = 1.0 / 1307674368000.0;
    s = s * -r2 + 1.0 / 6227020800.0;
    s = s * -r2 + 1.0 / 39916800.0;
    s = s * -r2 + 1.0 / 362880.0;
    s = s * -r2 + 1.0 / 5040.0;
    s = s * -r2 + 1.0 / 120.0;
    s = s * -r2 + 1.0 / 6.0;
    s = s * -r2 + 1.0;
    return r * s;
}

static inline double nc_cos(double x)
{
    return nc_sin(x + NC_HALF_PI);
}

static inline double nc_exp(double x)
{
    double k, r, e, p = 1.0, base;
    long n;

    if (x > 709.0)
        return 1e308 * 10.0;            /* overflow: inf */
    if (x < -745.0)
        return 0.0;
    k = nc__round(x * (1.0 / NC_LN2));
    r = (x - k * 0.69314718036912381649) - k * 1.90821492927058770002e-10;
    /* Taylor to r^12, |r| <= ln2/2 */
    e = 1.0 / 479001600.0;
    e = e * r + 1.0 / 39916800.0;
    e = e * r + 1.0 / 3628800.0;
    e = e * r + 1.0 / 362880.0;
    e = e * r + 1.0 / 40320.0;
    e = e * r + 1.0 / 5040.0;
    e = e * r + 1.0 / 720.0;
    e = e * r + 1.0 / 120.0;
    e = e * r + 1.0 / 24.0;
    e = e * r + 1.0 / 6.0;
    e = e * r + 0.5;
    e = e * r + 1.0;
    e = e * r + 1.0;
    /* times 2^k */
    n = (long)k;                        /* |k| <= 1075 here */
    base = n < 0 ? 0.5 : 2.0;
    if (n < 0)
        n = -n;
    while (n)
    {
        if (n & 1)
            p *= base;
        base *= base;
        n >>= 1;
    }
    return e * p;
}

static inline float nc_sinf(float x) { return (float)nc_sin(x); }
static inline float nc_cosf(float x) { return (float)nc_cos(x); }
static inline float nc_expf(float x) { return (float)nc_exp(x); }

static inline float nc_minf(float a, float b)
{
    return a < b ? a : b;
}

static inline float nc_maxf(float a, float b)
{
    return a > b ? a : b;
}

static inline float nc_clampf(float x, float lo, float hi)
{
    return x < lo ? lo : x > hi ? hi : x;
}

#endif
