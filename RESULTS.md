# NativeCall: measurements

**System:** Amiga 1200 + PiStorm32-lite + Raspberry Pi 4 Model B 8 GB (Cortex-A72 1.5 GHz), Emu68
`1.1.0-beta.1 git:0ba3a89` (unmodified), `armstub=nativecall-armstub.bin` in `config.txt`, AmigaOS 3.2,
Kickstart 47.115. Emu68 reports itself as a 68040.

All figures come from one machine. Each table states what was running at the time; versions moved on
during the project, so older tables were not repeated with the newest versions.

## Summary

| Workload | 68k (Emu68 JIT) | Native | Gain |
|---|---:|---:|---:|
| CRC32, 4 MB (hardware instruction) | 52.11 ms | 2.38 ms | 21.9x |
| 4x4 matrix × 10 000 vertices (NEON) | 1.01 ms | 0.11 ms | 9.2x |
| Mandelbrot zoom (NEON, 3 cores) | 3.2 fps | 27.1 fps | 8.8x (iterations/s) |
| Adler32, 4 MB (NEON) | 19.86 ms | 2.77 ms | 7.2x |
| Software 3D torus | 44.6 fps | 169.3 fps | 3.8x |
| Same C code, one core, no SIMD | | | 1.4x to 2.4x |
| GPU-limited demo (Boing Ocean) | 55.3 fps | 55.8 fps | +0.9% |

Emu68's JIT already turns 68k code into ARM code, so plain C gains about 2x. The big gains come from
what the 68k has no equivalent of (NEON, the CRC32 instruction, extra cores). Where the GPU sets the
frame rate, a faster CPU part gains almost nothing.

## `bench`: checksums and matrices (29.10.2026, hypervisor 1)

VBCC 0.9h `-O2 -cpu=68040 -fpu=68040`; native gcc 14 `-O2 -mcpu=cortex-a72 -mbig-endian
-mcmodel=tiny`. `timer.device` EClock, best of 5. 4 MB of random data; 10 000 vertices of 4 floats.
Native work in pieces of 128 KB.

| Test | Implementation | Time (ms) | Throughput | vs 68k | Native calls | Longest call (µs) | Check |
|---|---|---:|---:|---:|---:|---:|---|
| Adler32 | 68k | 19.86 | 201.4 MB/s | 1.0x | 0 | - | reference |
| Adler32 | native | 8.64 | 463.0 MB/s | 2.3x | 32 | 624 | ok f68bc8b0 |
| Adler32 | native + NEON | 2.77 | 1443.3 MB/s | 7.2x | 32 | 348 | ok f68bc8b0 |
| CRC32 | 68k (table) | 52.11 | 76.8 MB/s | 1.0x | 0 | - | reference |
| CRC32 | native (table) | 24.40 | 163.9 MB/s | 2.1x | 32 | 1924 | ok 9d4b4a5b |
| CRC32 | native + CRC32 insn | 2.38 | 1679.0 MB/s | 21.9x | 32 | 199 | ok 9d4b4a5b |
| Matrix 4x4 | 68k FPU | 1.01 | 151.4 MB/s | 1.0x | 0 | - | reference |
| Matrix 4x4 | native | 0.23 | 676.5 MB/s | 4.5x | 1 | 199 | ok (max rel diff 2.3e-07) |
| Matrix 4x4 | native + NEON | 0.11 | 1387.7 MB/s | 9.2x | 1 | 82 | ok (max rel diff 7.2e-06) |

- The BCM2711 has no ARMv8 crypto extension (PMULL), so CRC32 uses the CRC32 instruction instead of
  NEON.
- Native and NEON matrix results agree with the 68k FPU within a relative 1e-5 (NEON uses fused
  multiply-add; the 68k side computes in higher precision).
- "Longest call" here is read with the EClock around the call on the 68k, so it includes tasks and
  interrupts that run right after the call. Average call times were all below 1 ms: the longest,
  CRC32 table, is 24.40 ms / 32 = 0.76 ms.
- Multitasking keeps running: with 5 million native calls in the background, 5 × 16 MB were copied
  over the network (about 2.7 to 3 MB/s), the md5 sums matched, and Workbench kept responding.

**VBCC and GCC** (same kernels, library version; 4 MB, 128 KB pieces, best of 5):

| Test | VBCC 68k | GCC 16 68k | native | native + NEON / CRC32 insn |
|---|---:|---:|---:|---:|
| Adler32 | 19.90 ms | 17.89 ms | 8.6-8.8 ms | 2.8 ms |
| CRC32 (table) | 51.61 ms | 38.91 ms | 24.3-24.5 ms | 2.4 ms |
| Matrix 4x4 × 10 000 | 1.01 ms | 0.87 ms | 0.19 ms | 0.07 ms |

## `cputest`: the same C code on both sides

Latest run (02.11.2026, 68k side GCC 16, system idle):

| Test | 68k | native ARM | native NEON | Result check |
|---|---:|---:|---:|---|
| Primes below 3 000 000 (integer, division) | 1457.1 ms | 1020.4 ms (1.4x) | – | both 216 816 |
| Mandelbrot 640x480, 1000 iterations (double) | 1023.7 ms | 677.4 ms (1.5x) | 248.1 ms (4.1x, float × 4) | double identical (76 318 331); NEON 0.15% off |

With the 68k side built by VBCC the gains were larger, because its 68k code is slower: 2756.5 ms
against 1135.6 ms for primes (2.4x), 1224.2 ms against 697.6 ms for Mandelbrot (1.8x).

## `ncdemo`: game-like load, software 3D

640x480 RTG (Emu68 VideoCore, RGB565), rotating torus of 4096 triangles, z-buffer, Gouraud shading.
Kernels: `transform`, `raster` (line bands), `present` (palette to RGB565). 68k side GCC 16.

| Versions | 68k | native | Gain |
|---|---:|---:|---:|
| library 1.1, hypervisor 1 | 45.7 fps | 150.8 fps | 3.3x |
| library 1.2, hypervisor 2 | 44.6 fps | 169.3 fps | 3.8x |
| library 1.3, hypervisor 2 | 43.9 fps | 165.1 fps | 3.8x |

Successive runs vary between 165 and 169 fps. This demo uses `nc_run` (one core).

Per frame, library 1.2 (`ncdemo SECONDS=12 AUTO=6`):

| Mode | Frames | fps | transform | raster | display |
|---|---:|---:|---:|---:|---:|
| 68k | 267 | 44.6 | 0.37 ms | 18.60 ms | 3.48 ms |
| native | 1003 | 169.3 | 0.09 ms | 3.44 ms | 2.38 ms |

Native time per call, measured by the hypervisor; library 1.3 keeps calls under 1 ms by learning the
piece size:

| Kernel | Library 1.2: calls / avg / longest | Library 1.3: calls / avg / longest |
|---|---:|---:|
| transform | 1014 / 45 µs / 47 µs | 989 / 44 µs / 46 µs |
| raster | 6394 / 507 µs / 2477 µs | 8730 / 373 µs / **752 µs** |
| present | 1012 / 420 µs / 440 µs | 987 / 435 µs / 454 µs |

Learned on the way:

- `WriteChunkyPixels` to an AGA planar screen took about 400 ms per frame (conversion in chip RAM).
  On RTG, letting the OS convert 8-bit chunky to RGB565 took 24.7 ms; a `present` kernel plus
  `WritePixelArray(RAW)` takes 3 ms.
- When `NC_Run` restarted at piece size 1 on every call, native `transform` was slower than the 68k
  (0.64 ms against 0.37 ms). Remembering the size per kernel gives 0.13 ms.

## `mandeldemo`: Mandelbrot zoom, 1 and 3 cores

640x480 RTG, zoom into Seahorse Valley (view width 3.2 to 0.0002, 64 to 360 iterations). Each mode
runs alone through the same zoom cycle (`SECONDS=30 ZOOM=15 MODE=n`, 68k side GCC, system idle).

| Mode | Frames | fps | ms/frame | Miter/s | vs 68k |
|---|---:|---:|---:|---:|---:|
| 68k (Emu68 JIT, FPU, double) | 95 | 3.2 | 314.8 | 68.4 | 1.0x |
| native ARM (double) | 161 | 5.4 | 184.7 | 111.3 | 1.6x |
| native NEON (float × 4), 1 core | 311 | 10.4 | 94.6 | 221.6 | 3.2x |
| native NEON, 3 cores (`nc_run_mp`) | 812 | **27.1** | 35.2 | 599.5 | **8.8x** |

Three cores give 2.7 times one core. The rest is lost because core 0 prepares the request and waits
for the end, and the helpers take a moment to wake from `wfe`.

**Heat:** without cooling, a Pi 4 reaches 80 to 85 °C under this load and lowers its clock. On
02.11.2026 the same measurement gave 25.2 and then 24.0 fps; the one-core measurement stayed at
10.4 fps. `ENV:Emu68/Temperature` read 80.3 °C before and 83.7 °C after, `TemperatureMax` 85.0 °C.

An earlier run while a large file was being copied on the Amiga gave about half of these numbers in
every mode: other load on the machine matters.

## Call costs and asynchronous jobs (02.11.2026, library 1.5, hypervisor 4)

`hello TIME`, measured on the 68k with `nc_now_us`; averages of 1000 to 10 000 runs:

| | Time |
|---|---:|
| `nc_now_us()` (timer.device: about 20 µs) | 2.0 µs |
| `nc_addr_ok()` | 0.1 to 0.4 µs |
| empty request (no native code) | 4.1 to 4.9 µs |
| `nc_call(add)` round trip | 5.4 to 6.5 µs (8.9 µs in one VBCC run) |
| `nc_run`, 64 units | 7.2 to 7.9 µs |
| `nc_run_mp`, 64 units | 11 to 15 µs |
| `nc_start_mp` + `nc_wait`, 64 units | 15.0 µs (`nc_start_mp` 8.3 µs) |

The first call on a code path costs about 200 µs: Emu68's JIT translates the 68k code it has not seen
yet.

Same marking job of 16384 units (`hello MP`, `hello ASYNC`):

| Method | Time | Units per core 0/1/2/3 | 68k |
|---|---:|---|---|
| `nc_run` | 174.9 ms | 16384 / 0 / 0 / 0 | waits |
| `nc_run_mp` | 66.1 ms (2.65x) | 6142 / 5124 / 0 / 5118 | waits |
| `nc_start_mp` | 87.9 ms (2.0x) | 0 / 8192 / 0 / 8192 | **goes on running** |

During the asynchronous job the 68k ran 114 737 rounds of a test loop.

## Boing Ocean: when native calls do not help (02.11.2026)

ogles2.library's Boing Ocean example, 640x480 window, 300 frames, 68k side GCC. A fragment shader on
the GPU draws everything; the CPU's only per-frame work (`frame_setup`: the ball's float on the waves,
the sky light, the camera) was moved into a kernel.

| Version | fps | `frame_setup` |
|---|---:|---:|
| original example | 55.4 | – |
| kernel on the 68k | 55.3 | 82 µs |
| kernel on ARM | 55.8 | 44 µs |
| 68k, computed 20 times per frame | 53.7 | 777 µs |
| ARM, computed 20 times per frame | 55.1 | 291 µs |

The frame takes about 18 ms, nearly all of it on the GPU, so the frame rate barely moves. A repetition
of the calculation costs about 37 µs on the 68k and 13 µs on ARM: Emu68 already translates this kind
of double arithmetic well, and without NEON or extra cores the gain is 2 to 3 times.

This port is not part of this repository (it needs the ogles2 SDK).

## Real-world use: OpenMoHAA port (reported by that project, 02.11.2026)

Skeletal skinning moved into a kernel in an experimental OpenMoHAA port, same A1200, m1l1 truck scene:

| | fps | ms/frame |
|---|---:|---:|
| engine's own loop | 23.1 | 43.2 |
| kernel with NEON, one `nc_run_mp` batch per draw list | **27.8** | 36.0 |

An earlier version with one call per surface gained only 0.4 ms per frame: the gain came from reducing
the number of calls.
