/* cputest kernels: shared by the 68k program and cputest.narm */
#ifndef CPUTEST_KERNELS_H
#define CPUTEST_KERNELS_H

#include <nativecall/kernel.h>

struct CTParams
{
    struct NCRange range;   /* primes: numbers [from, to); mandelbrot: rows [from, to) */
    NC_U32 width;
    NC_U32 height;
    NC_U32 maxiter;
};

/* primes:      number of primes in the range (trial division)
   mandel:      sum of iteration counts of the rows, double precision
   mandel_neon: the same, natively 4 pixels at a time in single precision
                (on the 68k it is the double version) */
#define CPUTEST_KERNELS(K)  K(primes) K(mandel) K(mandel_neon)

NC_DECLARE(cputest, CPUTEST_KERNELS)

#endif
