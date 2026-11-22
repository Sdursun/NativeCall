# NativeCall SDK

Run the number-crunching parts of a 68k Amiga program on the ARM processor (Cortex-A72, NEON) of
the Raspberry Pi in a PiStorm/Emu68 system. The program runs on every Amiga: without native calls
the same code runs as 68k.

- **Kernels:** you write the computing functions **once**. The SDK compiles them twice: for the Pi
  (a `.narm` module) and for the 68k (the fallback).
- **68k side:** the rest of the program is an ordinary AmigaOS program, built with **VBCC** or
  **GCC**.
- **Build:** everything builds in Docker. Nothing else needs to be installed on Windows (Git Bash)
  or Linux.

## Quick start

```sh
sdk/nc image                    # once: Docker image nativecall-sdk:1
sdk/nc new myprog               # myprog/: main.c, myprog_kernels.h, myprog_kernels.c, Makefile
cd myprog
../sdk/nc make                  # VBCC -> build/vbcc/myprog (+ myprog.narm)
../sdk/nc make CC68=gcc         # GCC  -> build/gcc/myprog  (+ myprog.narm)
```

On the Amiga:

- Copy `nativecall.library` 1.8 to `LIBS:` (`sdk/build/nativecall.library`, built by `sdk/nc sdk`).
- **The program is one file:** the `.narm` module is embedded in it (`NC_EMBED=1`, the default). The
  `.narm` file next to it is optional (see "Single file").
- Native calls need the nativecall armstub on the Pi (../INSTALL.md), version 3 for several ARM
  cores, 4 for asynchronous jobs, 6 for the time limit, 7 for the log, fault details, memory
  protection and job cancel, 8 for statistics. With an older armstub the program still runs
  natively, without those. Without any armstub it runs its 68k version.
- **Under WinUAE** the kernels can run on the PC instead (`NC_X64 := 1` in the Makefile, the host DLL
  in WinUAE's `plugins` folder, "Allow native code" on): see "WinUAE: kernels on the PC" below.

**Coming from SDK 1.5.0 or 1.6.0:** rebuild your modules. Those SDKs left a module's `.bss` out of
the image, so a kernel with zero-initialised static or global variables wrote past its module
(../NOTES.md 26.7).

## How to write a program

The rule: **everything that talks to AmigaOS stays on the 68k, the computing goes into kernels.**

### 1. Kernels header: `myprog_kernels.h`

```c
#include <nativecall/kernel.h>

struct ScaleParams
{
    struct NCRange range;   /* first field for nc_run(): [from, to) */
    NC_U32 samples;         /* NC_S16 * (68k address), memory from nc_alloc() */
    NC_S32 factor;
};

#define MYPROG_KERNELS(K) \
    K(scale) \
    K(peak)

NC_CHECK_SIZE(struct ScaleParams, 16)              /* compiled on both sides */
NC_CHECK_OFFSET(struct ScaleParams, factor, 12)

NC_DECLARE(myprog, MYPROG_KERNELS)   /* NCK_scale = 0, NCK_peak = 1, prototypes */
```

The order of the list is the entry order of the `.narm` module. Both sides are generated from the
same list, so numbers never have to be matched by hand. `NC_CHECK_SIZE` / `NC_CHECK_OFFSET` verify
at compile time that the structure has the same layout under both compilers: a mismatch is a
compile error instead of silent data corruption.

### 2. Kernels: `myprog_kernels.c`

```c
#include "myprog_kernels.h"

NC_KERNEL(scale)
{
    struct ScaleParams *p = params;
    NC_S16 *s = NC_PTR(NC_S16, p->samples);
    NC_U32 i;

    for (i = p->range.from; i < p->range.to; i++)
        s[i] = (NC_S16)((s[i] * p->factor) >> 8);
    return 0;
}

NC_KERNEL(peak)
{
#if NC_NATIVE
    /* NEON version (#include <arm_neon.h> at the top of the file, inside #if NC_NATIVE) */
    return ...;
#else
    /* 68k version (or plain C) */
    return ...;
#endif
}

NC_MODULE(myprog, MYPROG_KERNELS)
```

### 3. Program: `main.c`

```c
#include <nativecall/nativecall.h>
#include "myprog_kernels.h"

struct NCKernels *k = nc_open(&myprog_kernels);     /* library + embedded module */
struct ScaleParams *p = nc_alloc(sizeof(*p));
NC_S16 *samples = nc_alloc(count * 2);

p->samples = (NC_U32)samples;
p->factor = 300;
nc_run(k, NCK_scale, p, 0, count);                  /* in slices of about 0.5 ms */
nc_run_mp(k, NCK_scale, p, sizeof(*p), 0, count);   /* the same, on the idle ARM cores as well */
peak = nc_call(k, NCK_peak, p);                     /* one call */

if (nc_is_native(k) && nc_status(k) != NC_ST_OK) ...  /* native error */

nc_free(samples, count * 2);
nc_free(p, sizeof(*p));
nc_close(k);
```

| Function | What it does |
|---|---|
| `nc_open(&set)` | Opens the library, loads the module (order: "Single file"). Falls back to the 68k otherwise. NULL only when out of memory. |
| `nc_open_path(&set, path)` | The same with a given `.narm` file (no version check). |
| `nc_call(k, i, p)` | Runs a kernel once. |
| `nc_run(k, i, p, from, to)` | Runs `[from, to)` in pieces of about 0.5 ms, adds up the results. The piece size is learned per kernel ("Measuring"). On the 68k: in one go. |
| `nc_run_mp(k, i, p, sizeof(*p), from, to)` | Like `nc_run`, also on the ARM cores Emu68 leaves idle (3 in total on a PiStorm; library 1.4 + hypervisor 3, otherwise `nc_run`). See "Several cores". |
| `nc_cores(k)` | ARM cores `nc_run_mp` uses (0: no native calls). |
| `nc_start_mp(k, &job, i, p, sizeof(*p), from, to)` / `nc_done` / `nc_wait` | **Asynchronous** `nc_run_mp`: the job runs on the helper cores while the 68k goes on (library 1.5 + hypervisor 4). See "Asynchronous jobs". |
| `nc_cancel(k, &job)` | Stops a job: pieces not started are skipped, running ones end, then like `nc_wait`; `nc_status` is `NC_ST_CANCELLED` (library 1.6 + hypervisor 7; otherwise it waits). |
| `nc_set_timeout(k, microseconds)` | Time limit for one native call or piece of this kernel set; 0 is the default of 1 s. Past it the call ends with `NC_ST_TIMEOUT` (library 1.6 + hypervisor 6). FALSE when there is no limit. |
| `nc_call_68k` / `nc_run_68k` | Always the 68k version (for comparisons). |
| `nc_is_native(k)` | Whether calls run natively. |
| `nc_status(k)` | Status of the last native call (`NC_ST_OK`, `NC_ST_FAULT`, ...); after `nc_open` also why there are no native calls (`NC_ST_MISMATCH`, `NC_ST_NOMODULE`, ...). See `nativecall/nc_abi.h`. |
| `nc_stats(k, i, &s, reset)` | **Native run time** of a kernel: calls, total / longest / last time (µs). Needs hypervisor 2 and library 1.2. |
| `nc_alloc` / `nc_free` | Memory a kernel can see (Fast RAM above 16 MB, cleared). |
| `nc_addr_ok(p, size)` | Can native code see this memory? Only compares, about 0.1 µs. Under WinUAE's x64 backend all of the Amiga's RAM (after `nc_open`). |
| `nc_addr_limits(&lo, &hi)` | The largest Fast RAM region native code can see, for two compares in hot loops. |
| `nc_now_us()` / `nc_now_ticks()` | A cheap clock. On Emu68 it reads the ARM counter (about 2 µs); timer.device costs about 20 µs per call on a PiStorm. Does not need `nc_open`. Under WinUAE's x64 backend it reads the PC's clock once `nc_open` found it: the emulation's own clocks miss the time a kernel runs. |

The functions of `nativecall.library` can be used directly too (`<proto/nativecall.h>`): `NC_GetInfo`
(hypervisor statistics), `NC_LastFault` (ESR/FAR/ELR of the last native fault), `NC_GetKernelStats`,
`NC_LoadModule`, `NC_LoadModuleMem`, `NC_Run`, `NC_RunMP`, `NC_Cores`, `NC_StartMP`, `NC_JobDone`,
`NC_WaitJob`, `NC_SetTimeout`, `NC_ReadLog`, `NC_GetFaultInfo`, `NC_CancelJob`, `NC_GetStats`,
`NC_ListModules`. Definitions: `sfd/nativecall_lib.sfd`.

| Library | New | Needs |
|---|---|---|
| 1.0, 1.1 | basic functions, `NC_Run` | hypervisor 1 |
| 1.2 | `NC_LoadModuleMem` (embedded module), `NC_GetKernelStats` | run times: hypervisor 2 |
| 1.3 | `NC_Run` learns the piece size per kernel | – |
| 1.4 | `NC_RunMP`, `NC_Cores` | several cores: hypervisor 3 |
| 1.5 | `NC_StartMP`, `NC_JobDone`, `NC_WaitJob` | asynchronous jobs: hypervisor 4 |
| 1.6 | `NC_SetTimeout`, `NC_ReadLog`, `NC_GetFaultInfo`, `NC_CancelJob`; unloading cancels jobs; 64 KB of slack behind a module | time limit: hypervisor 6; log, fault details, cancel: hypervisor 7 |
| 1.7 | `NC_GetStats`, `NC_ListModules` | statistics: hypervisor 8 |
| 1.8 | kernels on the PC under WinUAE (`NC_FEAT_X64`) | WinUAE with native code and the host DLL; no change with the hypervisor |

`ncsupport.c` checks the library version. With an older library `nc_run_mp` becomes `nc_run`,
`nc_start_mp` runs the job at once, `nc_stats` and `nc_set_timeout` return FALSE and `nc_cancel`
waits. `nc_now_us`, `nc_addr_ok` and `nc_addr_limits` do not depend on the library.

A program that exits without `nc_close` is cleaned up: the first `nc_open` registers an `atexit`
handler that cancels its jobs and unloads its modules. Programs without a C runtime compile
`ncsupport.c` with `-DNC_NO_ATEXIT` and must call `nc_close` themselves.

## Kernel rules

A kernel runs on the Pi, outside AmigaOS (../NOTES.md 9): with `nc_call` / `nc_run` on Emu68's core,
with `nc_run_mp` also on the cores Emu68 leaves idle (../NOTES.md 22).

1. **No AmigaOS calls, and no libc.** The native build (and the x64 build for WinUAE) has small SDK
   headers (`sdk/native/include`):
   - `string.h`: `mem*`, `strlen`, `strnlen`, `strcmp`, `strncmp`, `strcpy`, `strncpy`, `strcat`,
     `strchr`, `strrchr`, `memchr`;
   - `stdlib.h`: `malloc` / `calloc` / `realloc` / `free` (`nativecall/nc_heap.h`, below), `abs`.

   `limits.h`, `stdint.h`, `stddef.h`, `stdbool.h` and `float.h` come from the compiler. Floating
   point is fine; `nativecall/nc_math.h` has `nc_sin`, `nc_cos`, `nc_exp`, `nc_sqrtf`, `nc_floorf`
   and friends, the same code on both sides.
2. **Only Fast RAM above 16 MB.** `nc_alloc()` guarantees it. The program's own BSS or `AllocMem`
   memory is fine too if `nc_addr_ok()` says so. Chip RAM, Zorro II space and hardware registers are
   not visible to native code: an access comes back as `NC_ST_FAULT`, the machine does not lock up.
3. **Short calls (about 1 ms).** While native code runs on core 0, 68k interrupts and multitasking
   stop. Split big jobs with `struct NCRange` + `nc_run()`. `nc_stats()` says how long a kernel
   takes. The limit does not apply to asynchronous jobs on the helper cores (the 68k does not stop).
   - **Fixed cost per call** (A1200, `hello TIME`): `nc_call` round trip about 5.5 µs, `nc_run_mp`
     about 11 µs, `nc_start_mp` + `nc_wait` about 15 µs. Do not call for many tiny jobs one by one:
     **send them in a batch** ("The batch pattern").
4. **Parameter structures** are big-endian on both sides; no conversion is needed.
   - Every field must be 4 bytes: `NC_U32`, `NC_S32`, `float`, pointers as `NC_U32`. Byte or word
     arrays must have a length that is a multiple of 4. VBCC, GCC (with or without `-malign-int`)
     and ARM then all see the same layout.
   - No `double` fields: VBCC aligns them to 2 or 4 bytes.
   - Pin the layout with `NC_CHECK_SIZE` / `NC_CHECK_OFFSET`.
   - Read pointer fields with `NC_PTR()`.
   - **For WinUAE's x64 backend** (little-endian PC): structures defined after `kernel.h` keep the
     68k's layout by themselves, so params and arrays of such structures are right. Plain arrays of
     16-bit, 32-bit or float values reached through `NC_PTR()` are not converted: read and write them
     with `NC_GET16/32/F` and `NC_PUT16/32/F` (plain accesses on the 68k and the Pi). Do not take the
     address of a field of such a structure (`const float *r = c->rot`): the build stops with an
     error; index it instead. Bytes need nothing. See sdk/x64/README.md.
5. **A call has a time limit** (hypervisor 6): 1 s by default, `nc_set_timeout` changes it. A kernel
   that runs past it is stopped and the call returns `NC_ST_TIMEOUT`; with `nc_run_mp` and
   `nc_start_mp` the limit applies to each piece. The machine stays locked until then, so give
   kernels that might loop a short limit while you develop them. After a time-out the kernel's output
   may be half written and a lock it held (the `nc_heap` one, for example) stays taken: unload the
   module or report the error. With an older hypervisor, or an armstub built with `GIC=0`, an endless
   loop locks the machine for good.
   - **Stack:** every ARM core has a 64 KB stack; the hypervisor and the FPU save use about 1 KB.
     With hypervisor 7 an overflow hits a guard page and ends in `NC_ST_FAULT`. Keep a kernel's
     locals **under 32 KB**; large arrays belong in `nc_alloc` memory or a `nc_heap`.
   - **The 68k version runs on the caller's stack**, and a Shell gives a program 4 KB. See
     "Debugging".
6. **A module is at most 1 MB.** The SDK builds with `-mcmodel=tiny` and checks position independence
   in every build.
7. **68k and native results can differ.**
   - Emu68's 68k FPU computes in double (64 bit); a real 68040/060 keeps intermediate results in
     80 bit. With `float` in a kernel, borderline calculations (is a triangle in front of a plane or
     behind it) may come out differently on the two sides.
   - 68k code that allocates memory from a kernel's result must assume the worst case.
   - For bit-identical results use `double` on both sides and add
     `CFLAGS_NATIVE := -ffp-contract=off` (ARM uses fused multiply-add otherwise).
8. **Design for re-running after a fault.** When `NC_ST_FAULT` comes back, the kernel may have written
   part of its output.
   - A kernel that only writes an output buffer (and leaves its input alone) can be run again from
     the start on the 68k with `nc_call_68k`.
   - That is not possible for kernels that change state (for example a sound engine's internal
     state); those should write their state in one step at the end of the call.
9. **Kernels for `nc_run_mp`** run on several cores at once:
   - they do not write their params (each core gets its own copy, what it writes is lost);
   - each piece writes only the output of its own `[from, to)` range;
   - pieces do not read each other's results and run in no particular order;
   - no shared counter or total: return it instead (the results are added up).

### The batch pattern

A game's work often does not fit `nc_run` ranges: it is dozens of small jobs per frame (every model,
every surface). One `nc_call` for each loses the gain to the fixed cost per call. In the OpenMoHAA
port, one call per surface gained 0.4 ms per frame; the same work as a batch took the game from 23.1
to 27.8 fps.

The pattern: collect the jobs into an array during the frame and let a kernel process the array with
an `NCRange`:

```c
struct SkinJob { NC_U32 verts, count, out; /* ... */ };
struct SkinParams { struct NCRange range; NC_U32 jobs; };   /* jobs: struct SkinJob[] */

NC_KERNEL(skin)
{
    struct SkinParams *p = params;
    struct SkinJob *job = NC_PTR(struct SkinJob, p->jobs);
    for (NC_U32 i = p->range.from; i < p->range.to; i++)
        /* job[i] */;
    return 0;
}

nc_run_mp(k, NCK_skin, p, sizeof(*p), 0, job_count);       /* one or two calls per frame */
```

If the jobs differ in size, the number of pieces in `nc_run_mp` balances them; a single job should
still stay under about 1 ms.

### Memory in a kernel: `nc_heap`

Compiling an existing library (for example a decoder) into a kernel needs `malloc`.
`nativecall/nc_heap.h` is a small allocator working in a block of memory the 68k provides:

```c
/* 68k */    p->heap = (NC_U32)nc_alloc(256 * 1024);    /* nc_alloc clears it: no heap yet */
/* kernel */
struct NCHeap *h = nc_heap_open(NC_PTR(void, p->heap), 256 * 1024);   /* the existing one, or new */
nc_heap_select(h);          /* natively: malloc()/free() use this heap */
void *buf = nc_heap_alloc(h, 4096);
nc_heap_free(h, buf);
```

- The heap lives in the block and survives between calls.
- It holds offsets, not pointers, so the same heap also works in the kernel's 68k version.
- It has a lock: pieces of `nc_run_mp` may allocate at the same time.
- `malloc()` goes to the heap only in the native and x64 builds; the 68k build uses the compiler's own.
- `hello HEAP` tests that both sides give the same result.

## Makefile

```make
NAME          := myprog          # program name
SRCS          := main.c gui.c    # 68k sources
KERNELS       := myprog_kernels.c
MODULE        := myprog          # the NC_MODULE() prefix (default: NAME)
CC68          ?= vbcc            # vbcc | gcc
CFLAGS68      :=                 # extra 68k flags
LIBS68        :=                 # extra 68k libraries
CFLAGS_NATIVE :=                 # extra flags for the kernels' Pi build
OPT68         := -O2             # 68k optimisation level (-O1 / -O0 if VBCC chokes on a source)
NC_EMBED      ?= 1               # 1: the module is embedded in the program
NC_X64        := 1               # also build the kernels for WinUAE's x64 backend (default 0)
NC_EMBED_X64  ?= 1               # 1: that module is embedded too
CFLAGS_X64    :=                 # extra flags for the kernels' x64 build
NC_SDK        ?= ../sdk
include $(NC_SDK)/mk/nativecall.mk
```

Targets: `nc make` (program + module), `nc make narm` (only the module and `<prefix>_image.c`),
`nc make nx64` (only the x64 module), `nc make clean`. A changed header rebuilds everything.

| Compiler | Flags |
|---|---|
| VBCC (0.9h P3) | `+aos68k -c99 -O2 -cpu=68040 -fpu=68060`, NDK 3.2, `-lm040 -lamiga` |
| GCC 16.2 | `-O2 -m68040 -mtune=68060 -mhard-float -fno-branch-count-reg -noixemul` (libnix), `-lm` |

With this GCC, `-m68040` alone does not enable the FPU: without `-mhard-float` floating point runs in
software and is 100 times slower on Emu68. The SDK adds the flag itself.

`-fpu=68060` / `-mtune=68060` and `-fno-branch-count-reg` are there for Emu68: float to integer
conversions use `FINTRZ` (the 68040 way needs three FPCR accesses, which are expensive in the JIT),
and loops are not closed with `DBF` (the JIT ends its translation unit there). The 68k path of
`ncdemo` became twice as fast with them (../NOTES.md 27). The code still runs on a real 68040, where
68040.library emulates `FINTRZ`. If your project builds its 68k side itself, the same flags are
worth trying.

Kernels are built for the Pi with `-g -fno-omit-frame-pointer`, so that a fault can be traced back
to source lines ("Debugging"). The `.narm` does not grow; the information stays in the `.elf`.

## Single file: embedded module and build ID

`nc make` turns the module into C data with `tools/narm2c.py` (`build/native/<prefix>_image.c`) and
links it into the program, along with the module's CRC32: the **build ID**. `nc_open` looks for the
module in this order:

1. **`ENV:NativeCall/ModuleDir`, if set:** `<prefix>.narm` in that directory, no version check. For
   trying a new module during development without relinking: `SetEnv NativeCall/ModuleDir RAM:test`.
2. **The copy embedded in the program.** Always the right version.
3. **`PROGDIR:<prefix>.narm`.** Used when there is no embedded copy (`NC_EMBED=0`). If its CRC32 does
   not match the build ID it is not loaded (`nc_status` = `NC_ST_MISMATCH`) and the program falls back
   to the 68k. An old `.narm` can thus never silently run with a new program.

The embedded copy is also loaded into memory on machines without a PiStorm (the size of the module,
usually a few KB). Use `NC_EMBED=0` to avoid that.

With `NC_X64 := 1` the x64 module (`.nx64`) is embedded as well (`NC_EMBED_X64`, 8 to 12 KB for the
examples). Under WinUAE's x64 backend `nc_open` hands the library that copy in step 2; without one
the library looks for a `.nx64` file that belongs to the `.narm` in `ENV:NativeCall/ModuleDir` and
`PROGDIR:`.

Library 1.1 cannot load from memory, so with it the module always comes from a file, unchecked.

## Adding it to an existing project

In a project whose 68k side is built by its own compiler (for example bebbo GCC + clib2),
`nativecall.mk` is not used for the program, only for the module:

1. Write the kernels header and file as above.
2. A small Makefile (`NAME`/`SRCS` are not needed):
   ```make
   MODULE  := game
   KERNELS := game_kernels.c
   NC_SDK  ?= ../sdk
   include $(NC_SDK)/mk/nativecall.mk
   ```
   `nc make narm` produces `build/native/game.narm` and `build/native/game_image.c`.
3. Compile and link with the project's own compiler:
   - `sdk/lib68k/ncsupport.c`,
   - the kernels file (as the 68k fallback),
   - `game_image.c`.

   Include path: `sdk/include`.
4. Without `game_image.c`, compile the kernels file for the 68k with `-DNC_NO_IMAGE`. The module is
   then loaded from a `PROGDIR:` file, without a version check.
5. Coming from SDK 1.7.0: regenerate `game_image.c` (`nc make narm`). It now also holds the x64
   module (empty without `NC_X64 := 1`), and `kernel.h` expects its two new symbols.

## Measuring

While native code runs, 68k interrupts stop, so a timer or a VBlank-driven profiler on the 68k side
cannot see native time. Instead the hypervisor (version 2+) measures each call with the ARM counter:

```c
struct NCKernelStats s;

if (nc_stats(k, NCK_scale, &s, TRUE))      /* TRUE: reset after reading */
    printf("scale: %lu calls, avg %lu us, longest %lu us\n",
           s.calls, s.total_us / s.calls, s.max_us);
```

If `max_us` goes above about 1000, split the work further. `nc_run` adjusts its piece size from the
same measurements (target 500 µs). With hypervisor 1 `nc_stats` returns FALSE and `nc_run` falls back
to the old method (timer.device EClock).

**How `nc_run` learns the piece size (library 1.3):**

- During the **first** `nc_run` of a kernel, the piece grows while it stays short.
- Later calls start with the learned size and only shrink during the call. After a piece longer than
  750 µs the size shrinks at once, in proportion to the time.
- At the end of a call:
  - if all pieces took less than 250 µs, the size doubles;
  - if one took too long, the smallest size stays.

The reason: the cost of a piece often depends on where it is (the empty top of the screen, then the
busy middle). A size that grew on cheap pieces runs into a long call within the same run. For games
processing the same range every frame, the size settles within a few frames.

The same applies to `nc_run_mp`, with its own piece size; the time measured there is until all cores
have finished.

## Several cores: `nc_run_mp` (library 1.4, hypervisor 3)

Of the Pi 4's four cores, Emu68 gives one to its JIT and one to PiStorm work (the housekeeper). The
other two wait idle. Hypervisor 3 uses those two for native work. Emu68 itself does not change
(../NOTES.md 22).

Helper cores take work only **while they are idle**, that is, while Emu68 waits in `wfe` on them.
Depending on `CMDLINE.TXT`:

| Setting | Core 1 | Core 3 |
|---|---|---|
| neither (default) | always helps | always helps |
| `async_log` | writes the serial log; helps while the queue is empty (almost always), the log may be delayed slightly | – |
| `ppc_enable` | – | PowerPC emulation; helps only while the PPC program sits in an idle loop |

This behaviour comes from the Emu68 source; those two settings have not been tested. A busy core is
never waited for: the job finishes on the cores at hand, at least core 0. `nc_cores(k)` counts the
captured cores (3), not how many are helping right now.

```c
r = nc_run_mp(k, NCK_raster, p, sizeof(*p), 0, height);
```

- **Splitting:** the range is cut into calls of about 0.5 ms as in `nc_run`. Within a call the
  hypervisor makes smaller pieces; all cores take pieces from the same counter.
- **Waiting:** the 68k waits for the end of the call (the result and the output are ready). There is
  no parallelism between the 68k and native code here, only among the native cores.
- **`params_size`:** the size of the params structure, 8 to 256 bytes, a multiple of 4.
- **Fallback:** without native calls it runs on the 68k in one go; with hypervisor < 3 or library <
  1.4 as `nc_run`. The program does not change.
- **Faults:** after a fault on any core no more pieces are handed out and `NC_ST_FAULT` comes back
  (the first fault's information).

**Gain** (A1200, ../RESULTS.md):

| Job | 1 core | 3 cores |
|---|---:|---:|
| `hello MP` (16384 units) | 174.9 ms | 66.1 ms (2.65x) |
| `mandeldemo` NEON | 10.4 fps | 27.1 fps (2.6x) |

It helps for lines, pixels or samples that are independent of each other: graphics, image and sound
filters, physics, independent blocks of compression. For small jobs (below about 50 µs in a call) the
splitting costs more than it gains; `nc_run` is enough there.

## Asynchronous jobs: `nc_start_mp` (library 1.5, hypervisor 4)

`nc_run_mp` shares the ARM work but the 68k waits for it. `nc_start_mp` gives the job to the helper
cores only and returns at once: the 68k (game, sound, drawing) goes on, and interrupts and
multitasking do not stop.

```c
struct NCJob job;

nc_start_mp(k, &job, NCK_decode, p, sizeof(*p), 0, frames);    /* about 8 µs, returns */
/* ... other 68k work: leave p, the input and the output alone ... */
if (nc_done(k, &job)) ...                                      /* no waiting */
r = nc_wait(k, &job);                                          /* the result; nc_status(k) */
```

- **Rules:** those of `nc_run_mp`. In addition, the 68k must not touch the job's params, input or
  output until `nc_wait` returns.
- **`nc_wait` is required** even when `nc_done` says TRUE: that request is the memory barrier that
  orders the helpers' writes for the 68k.
- **Waiting:** if the job has not finished, core 0 helps during `nc_wait` in steps of at most 0.5 ms;
  the 68k runs in between.
- **At most 4 jobs at a time.** When all are taken `nc_start_mp` returns FALSE and runs the job at
  once (with `nc_run_mp`); the same without hypervisor 4 or library 1.5. `nc_wait` gives the result
  in every case, so the program does not change.
- **Piece size:** the size `nc_run` learned for this kernel, otherwise 1/16 of the range. A piece may
  take longer than 1 ms on a helper, but core 0 also takes pieces during `nc_wait`: keep them around
  0.5 ms.
- **Cancelling:** `nc_cancel(k, &job)` skips the pieces that have not started, lets the running ones
  end, and returns what `nc_wait` would; `nc_status(k)` is `NC_ST_CANCELLED` and the result is the
  sum of the pieces that ran. Cancelling an 87 ms job took 1.0 to 1.5 ms on the A1200.
- **`nc_close`** cancels unfinished jobs before the module goes away (library 1.6 + hypervisor 7;
  older ones wait for them). So does exiting without `nc_close`, and the library cancels the jobs
  of tasks that are gone when the next job starts. Only the memory is still yours to keep: do not
  free a job's buffers before `nc_wait`, `nc_cancel` or `nc_close` has returned.

A1200 (`hello ASYNC`, 16384 units): the job finished in 87.9 ms (174.9 ms on one core), the 68k kept
running all the time, and only cores 1 and 3 did the work.

Uses: decoding (MP3) and mixing sound, skeleton poses, preparing the next frame: any job whose result
the 68k does not need right away.

## WinUAE: kernels on the PC (library 1.8)

Under WinUAE there is no Pi. With `native_code=true` and `nativecall-windows-x86-64.dll` in WinUAE's
`plugins` folder, library 1.8 runs the kernels on the PC's processor instead. The program does not
change and does not know: `nc_call`, `nc_run`, `nc_run_mp`, `nc_start_mp`, `nc_cancel`, time limits,
the log, fault details and statistics all work; `NCLog STATS` and `NCStatus` say "WinUAE x64".

- **Build:** `NC_X64 := 1`. The kernels file is also compiled with MinGW-w64 into a small Windows DLL
  without a C library (SDK headers and runtime as on the Pi), and embedded in the program.
- **Byte order:** see rule 4 above (`NC_GET32` & co.). All examples use the accessors.
- **Cores:** `nc_run_mp` and `nc_start_mp` use a thread pool of all the PC's logical processors but
  one; `nc_cores` says how many. The emulation stands still during `nc_call` / `nc_run` /
  `nc_run_mp`, not during `nc_start_mp` jobs.
- **Faults:** crashes, stack overflows and endless loops end the call with `NC_ST_FAULT` or
  `NC_ST_TIMEOUT`; WinUAE goes on. `nc addr2line build/x64/<module>.dbg.dll <offsets>` turns
  `NCLog FAULT`'s backtrace into source lines.
- **Speed** (WinUAE 6.1 beta, 24 logical processors): `mandeldemo` 640x480 6044 Miter/s on the pool
  against 58.5 on WinUAE's 68k JIT.

Everything about it, with measurements and known WinUAE problems: [x64/README.md](x64/README.md).

## Examples (`examples/`)

| Example | What it shows |
|---|---|
| `hello` | `add(40,2)`, 1000 calls, a kernel using `memcpy`, a kernel that crashes on purpose (`FAULT`). `MP`: how much each ARM core did. `ASYNC`: `nc_start_mp`. `HEAP`: `nc_heap` on both sides. `TIME`: call costs. `TIMEOUT`: an endless kernel is stopped. `LOG`: `nc_printf` and a backtrace. `PROTECT`: what native code may not touch. `CANCEL`, `LEAK`: stopping a job, exiting without cleaning up. |
| `cputest` | Primes and Mandelbrot: 68k / native / NEON, in the Shell with bar charts |
| `bench` | Adler32, CRC32 (NEON, CRC32 instruction), 4x4 matrix; the table in ../RESULTS.md |
| `ncdemo` | A game-like load: software 3D with a z-buffer (rotating torus, 4096 triangles) on an RTG screen; `N` or a click: 68k, native, all cores (`nc_run_mp`); fps on screen |
| `mandeldemo` | Mandelbrot zoom (Seahorse Valley) on an RTG screen; `1`-`4` or `N`: 68k / native double / **NEON** (float, 4 pixels at once) / **NEON on all cores** (`nc_run_mp`); iterations per second on screen. An example of NEON intrinsics and `nc_run_mp`. |

| `raydemo` | Real-time ray tracing: mirror spheres, a chequered floor, shadows, 3 bounces. Independent work per pixel, so the gain is the largest here (A1200: 68k 1.9 fps, native 11, three cores 34). |
| `voxeldemo` | Flight over a height map (one ray per column); a kernel also generates the map at start. 198 fps on three cores. |
| `nbodydemo` | Gravity between 1500 bodies, every pair (2.25 million pairs per frame). A step with two buffers: each piece reads only the old state and writes its own entries. |
| `filterdemo` | Image processing: bilinear rotation and scaling, then a 3x3 filter (blur, sharpen, edges, emboss; key `F`). Two kernels in a row. |
| `mp3demo` | MP3 player: decoding (`minimp3`) with `nc_start_mp` on an idle ARM core, the 68k only hands buffers to AHI. `BENCH`: decoding time on the 68k and natively, samples compared. |

`raydemo`, `voxeldemo`, `nbodydemo` and `filterdemo` share a skeleton (`examples/common/`):
`demofw.c` opens the screen (16-bit RTG, else 32-bit, else 256 colours), switches between 68k, native
and all cores with `1`-`3` / `N`, measures, and prints a summary at the end; `demopix.h` lets kernels
write in the screen's pixel format. Common arguments: `WIDTH HEIGHT SECONDS AUTO MODE`. A new graphics
demo only needs a `struct DemoApp` (init, frame, cleanup).

```sh
cd sdk/examples/cputest && ../../nc make && ../../nc make CC68=gcc
```

## Games and graphics

`examples/ncdemo` shows the pattern for games:

- **Kernels** (transform, raster, present) draw only into buffers in Fast RAM.
- **The 68k side** copies the result to the screen. On RTG it does so with
  `WritePixelArray(RECTFMT_RAW)`; a kernel also converts to the screen's pixel format.
- **Line bands:** the whole frame is split into bands by `nc_run`, so a call stays around 0.5 ms and
  sound interrupts are not disturbed.
- **Writing to chip RAM is expensive.** `WriteChunkyPixels` to an AGA planar screen took about 400 ms
  per frame. The demo looks for an RTG mode (`DIPF_IS_FOREIGN`) first.
- **GPU-limited programs gain little.** If the frame time goes to the GPU, a faster CPU part does not
  change the frame rate (`RESULTS.md`, Boing Ocean).

## Status: `NCStatus` and `NCLog STATS` (library 1.7, hypervisor 8)

- **`NCStatus`** (`dist/Tools/`, MUI 3.8+): whether NativeCall is active (hypervisor and library
  version, whether the time limit works), the share of its time each ARM core spends in native code,
  the SoC temperature, the programs that have native modules loaded (calls per second, ARM time,
  jobs, faults) and the hypervisor's log. It says so if the library or the hypervisor is missing. It
  has an icon; start it from Workbench or the Shell.
- **`NCLog STATS`:** the same in the Shell, from a one-second sample.
- **From a program:** `NC_GetStats(&stats)` (`struct NCStats`: native time and calls per core,
  temperature, jobs, faults) and `NC_ListModules(info, max)` (`struct NCModuleInfo`: the name of the
  program that loaded the module, calls, ARM time). Both are in `nc_abi.h`. Load is the difference
  between two readings: change of `busy` / change of the counter.
- Core 0 belongs to the 68k (Emu68's JIT); the share shown is the part spent in native code. The
  helper cores (1 and 3 on a PiStorm) are busy only while they run pieces of `nc_run_mp` /
  `nc_start_mp`.

## Debugging

- **`nc_status(k)`:** `NC_ST_FAULT` means the kernel crashed, `NC_ST_TIMEOUT` that it ran past its
  time limit.
- **Log** (hypervisor 7, library 1.6): in a kernel, `nc_printf("x=%d\n", x)` / `nc_log("...")` write
  to a 16 KB ring in the hypervisor (from any core, at most 255 bytes per call; in the 68k version
  they do nothing). `nc_printf` knows `%d %i %u %x %X %p %s %c`, the flags `-` and `0`, a width, and
  `l` / `ll` / `z`. On the Amiga `NCLog` (or `NCLog FOLLOW`) prints the log; a program reads it with
  `NC_ReadLog()`. The hypervisor adds a line of its own for every fault and time-out.
- **Fault details:** `NCLog FAULT` (or `NC_GetFaultInfo()`) gives the registers of the last fault and
  a backtrace as module offsets. Turn them into source lines on the PC:
  `nc addr2line build/native/<module>.elf 0x30 0x4c 0x6c 0xeb0` → `df3 at ..._kernels.c:167`, ...
  Under WinUAE's x64 backend: `nc addr2line build/x64/<module>.dbg.dll <offsets>`; the registers are
  the PC's (`rax` ... `r15`) and the exception is a Windows one (`C0000005` access violation,
  `C00000FD` stack overflow).
- **`NC_LastFault()`:** the older way: FAR and the low 32 bits of ELR.
- **`NC_GetInfo()`:** trap, call and fault counters.
- **Stack of the 68k version:** the 68k version of a kernel runs on the caller's stack, and a Shell
  gives 4 KB. A kernel with a large local array (an MP3 decoder: 16 KB), or libnix `printf` plus
  graphics calls, goes past that and silently corrupts memory. On the native side the stack is
  64 KB. Keep local arrays small, or run the work in a process with a big stack
  (`examples/common/bigstack.c`: `run_with_stack(fn, 65536, name)`). Do not rely on the compilers'
  `__stack` variable.
- **MUI programs with GCC 16:** the inline `MUI_NewObject` of the MUI SDK header builds a broken tag
  list. Define `NO_INLINE_STDARG` and use a version that copies the tags (`tools/ncstatus.c`).
  libnix `printf` reads `%u` as 16 bit: use `%lu` and a cast.
- **VBCC and a big third-party source:** if the optimiser fails or miscompiles (minimp3 at `-O1` and
  `-O2`), set `OPT68 := -O0` for VBCC in that project's Makefile, and compare 68k and native results.
- **`PiReboot NOW`** (`dist/C/`): restarts the Pi through its watchdog, for example after replacing
  the armstub; no power cycle is needed. It is a hard reset.
- **`nc_cores(k)` is 1?** The armstub is not hypervisor 3 or newer.
- **`nc_run_mp` slower than expected?** With `ppc_enable` core 3 does not help while a PowerPC program
  runs (`hello MP` shows how much each core did). Also check the temperature, below.
- **Profiling:** timer.device costs about 20 µs per call on a PiStorm (it reaches the CIA over the
  Amiga bus); use `nc_now_us()` (about 2 µs) for fine measurements. Sampling profilers attribute
  native time wrongly: Emu68 takes interrupts between JIT blocks. Use `nc_stats` for native time.
- **Heat:** a Pi 4 lowers its clock at 80 °C. Three cores at full load reach that without cooling
  (A1200: 85 °C, `mandeldemo` 27 → 24 fps). See `ENV:Emu68/Temperature` and
  `ENV:Emu68/TemperatureMax`.
- **No native calls?** `nc_status(k)` right after `nc_open` gives the reason:
  - `NC_ST_NONE`: no library or armstub, or the CPU is not a 68040/060.
  - `NC_ST_MISMATCH`: the `.narm` does not belong to this program.
  - `NC_ST_NOMODULE`: module not found or not loadable.
- **Docker:** `nc` says so and stops when Docker is not running; start Docker Desktop.
- **Changing the hypervisor:** first `qemu/run.sh hv`, then real hardware (../qemu/README.md).

## Directories

```
sdk/
  nc                  command: new, make, sdk, image, shell, addr2line
  docker/Dockerfile   nativecall-sdk:1 (GCC 16.2, VBCC 0.9h P3, NDK 3.2, aarch64 GCC 14, MinGW-w64)
  include/
    nativecall/       kernel.h, nativecall.h, nc_abi.h, nc_math.h, nc_heap.h
    proto/ clib/ inline/   nativecall.library (VBCC and GCC; clib/inline are generated from the sfd)
  sfd/                nativecall_lib.sfd
  library/            nativecall.library source
  lib68k/             ncsupport.c (nc_open, nc_call, ...; compiled into every program)
  native/             narm.ld, narm_rt.c, include/ (string.h, stdlib.h for kernels)
  mk/nativecall.mk    project rules
  tools/narm2c.py     turns a module into C data (embedding, build ID; also the .nx64)
  tools/mknx64.py     packs the x64 DLL of a module into its .nx64
  x64/                ncx_host.c: the WinUAE host DLL; README.md
  tools/nclog.c       NCLog: log, last fault, statistics
  tools/ncstatus.c    NCStatus (MUI); tools/mkicon.py draws its icon
  tools/pireboot.c    PiReboot: restart the Pi from the Amiga
  template/           the nc new template
  examples/           hello, cputest, bench, ncdemo, mandeldemo, raydemo, voxeldemo, nbodydemo,
                      filterdemo, mp3demo; common/ (the skeleton of the graphics demos)
```
