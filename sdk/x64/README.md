# Kernels on the PC under WinUAE (x64 backend, library 1.8)

Without the Pi hypervisor, `nativecall.library` 1.8 can run kernels on the PC's x64 CPU when the
Amiga is emulated by WinUAE. The same program runs on the Pi, under WinUAE and on any other Amiga:
programs built with this SDK carry their x64 module inside, as they carry their `.narm`. Programs
built with an older SDK (v1.7.0) use it too, unchanged, when their `<prefix>.nx64` lies next to
them.

## What is needed

- WinUAE with `native_code=true` (Settings > Host > Miscellaneous > Allow native code), which gives
  the Amiga `uaenative.library`. 64-bit WinUAE (`winuae64.exe`).
- `nativecall-windows-x86-64.dll` (`sdk/build`, `dist/WinUAE`) in WinUAE's `plugins` folder, once.
- `nativecall.library` 1.8 in `LIBS:`.
- Nothing per program when it was built with `NC_X64 := 1` (the module is embedded). Otherwise its
  `<prefix>.nx64` next to it (or in `ENV:NativeCall/ModuleDir`).

## How it works

- With no hypervisor, the library opens `uaenative.library` and the host DLL; `NC_GetStats` then
  reports `NC_FEAT_X64`. `nc_open` sees that and hands the library the `.nx64` embedded in the
  program. Without one it loads the `.narm` as on the Pi (embedded or a file): the library computes
  the CRC32 of the `.narm` (its build ID) and looks in `ENV:NativeCall/ModuleDir` and `PROGDIR:` for
  a `.nx64` with that ID and the same number of kernels. A `.nx64` is only used with the `.narm` it
  was built with.
- The library hands the `.nx64` to the host DLL, which writes the DLL it contains to
  `%TEMP%\nativecall\` under a name of its own and loads it: every load is its own instance, with
  its own static variables, as on the Pi. The file is deleted when the module is unloaded (and
  leftovers when WinUAE starts the next time).
- `nc_call`, `nc_run` and `nc_run_mp` go to the host DLL through `uaenative.library`'s
  `call_function`. `nc_run_mp` runs on a thread pool: the logical processors of the PC less one
  (left for WinUAE's own threads), started by the first `nc_run_mp`. A worker that finished a
  piece waits 100 us for the next one without sleeping (`NC_Run` sends them back to back), then
  sleeps. When the library is expunged it ends the pool (`ncx_shutdown`) and the DLL can be
  unloaded.
- `nc_start_mp` jobs (up to four at a time, as on the Pi) run on the pool while the emulation
  goes on; the job takes a copy of its params when it starts. `nc_done` asks the DLL, `nc_wait`
  lets the emulation thread help for at most 0.5 ms at a time with the 68k running in between,
  `nc_cancel` skips the pieces that have not started. A job that is still running when the Amiga
  is reset runs to its end and writes into the memory of the new boot: keep jobs short. Kernels
  reach Amiga
  memory at WinUAE's host address of it (`NC_PTR`); the DLL checks that a block is one piece of
  host memory.
- The emulation stands still while a kernel runs. The Amiga's clocks do not see that time; the
  run times in `nc_stats` are the PC's.

## Building a module

```make
NC_X64       := 1    # in the project Makefile: "nc make" also builds build/<cc>/<prefix>.nx64
NC_EMBED_X64 := 1    # the default: the .nx64 goes into the program too
```

or `nc make nx64` for the module alone. The kernels file is compiled with MinGW-w64 GCC (in the
`nativecall-sdk:1` image) against the x64 branch of `nativecall/kernel.h`:

- **No C library, as on the Pi:** the SDK's `string.h` and `stdlib.h` (`sdk/native/include`, `malloc`
  on a `nc_heap`), its runtime `narm_rt.c` (`mem*`, `str*`, `nc_printf`), `nc_math.h`, and libgcc
  only for the stack probe of large frames. The DLL imports nothing and is 8 to 12 KB for the
  examples (30 KB with the MP3 decoder); embedding it adds that much to the program, also on the
  Pi, where it is not used. `NC_EMBED_X64 := 0` leaves it out (then copy the `.nx64` next to the
  program).
- `NC_NATIVE` is 0 and `NC_X64` is 1: kernels take their plain C paths (no NEON).
- Structures defined after `kernel.h` keep the 68k's big-endian layout (GCC's
  `scalar_storage_order`), so params are read as they are. **Data reached through `NC_PTR()` is not
  converted.** A kernel that computes with 16-bit, 32-bit or float values from such buffers gives
  wrong results on the PC. That is why `NC_X64` is off by default. Kernels that only move bytes, or
  read their params and write bytes (`mandeldemo`), are fine.
- `CFLAGS_X64` adds flags for the x64 compile.
- Plain arrays of 16-bit, 32-bit or float values in 68k memory are read and written with
  `NC_GET16/32/F` and `NC_PUT16/32/F` (`nativecall/kernel.h`): plain accesses on the 68k and the Pi
  (the `.narm` files of all examples came out byte for byte the same), byte swaps on the PC.
  `nc_to_68k16(p, n)` converts a buffer a library wrote (a decoder's PCM output);
  `NC_LOCAL_STRUCTS_BEGIN/END` around an included library keeps its own structures in the PC's
  layout. All examples use them now and build their `.nx64` (`NC_X64 := 1`).

## Measured (WinUAE 6.1 beta, 68060 JIT, PC with 24 logical processors)

`mandeldemo`, the v1.7.0 binaries unchanged, with library 1.8 (Miter/s, in brackets the factor
over the 68k):

| | 68k | x64, 1 thread | x64, all threads |
|---|---:|---:|---:|
| 640x480, threads started per call | 54.2 | 580.5 (10.7x) | 63.4 (1.2x) |
| 640x480, thread pool (24 threads) | 58.5 | 511.9 (8.8x) | 6043.6 (103x) |
| 320x240, thread pool (23 threads) | | 497 | 6847 to 7080 |

With the pool, `mandeldemo` draws 390 frames a second at 640x480 and 710 at 320x240. A test that
runs the same kernel without drawing computed about 15 000 frames in 3.5 minutes on the pool,
every frame identical to the one-thread result (11 of 307 200 pixels differ from the 68k:
WinUAE's 80-bit FPU against the PC's double).

**The demos reset the Amiga or crash WinUAE 6.1 beta 22 now and then, with or without NativeCall.**
With WinUAE's GDB server and `cpu_halt_auto_reset=false` the CPU was caught halted at `$FFFFFDCC`
with A6 = 0: address 4 (the ExecBase pointer) had been cleared, and exec called itself through a
zero base. The crash dumps (12, 13 and 14 November without the x64 backend, 21 November with it)
are the same thing: `winuae64.exe+0x12cdf3a` reading `$FFFFFEDA` (FindTask, -294, through base 0).
`raydemo` in 68k mode reset the Amiga with no NativeCall library loaded at all. Kernels did not
write to `$0-$3FF` (checked around every call); one dump has a kernel writing to `$FFFFFFFF`, with
the system already failing. The CPU does not write address 4 (a GDB watchpoint, with and without
the JIT, never fired), and it only happens on the 640x480 AGA screen the demos get there
(chunky-to-planar with `WriteChunkyPixels`): on a 1280x720 RTG screen (`WIDTH=1280 HEIGHT=720`)
57 runs, 68k and x64, on one thread and on all of them, had no trouble. Use an RTG screen for tests
under WinUAE. Native kernels make the demos run more frames and meet it sooner. The same demos run without trouble on the A1200: stability tests
belong on real hardware.

Diagnostics of the host DLL: create `%TEMP%\nativecall\debug` (an empty file) and restart WinUAE;
`%TEMP%\nativecall\ncx.log` then gets kernels that change the Amiga's first KB, crashes inside a
kernel module with the last call, and the last 4096 calls whenever the DLL is unloaded or opened
again.

## Faults, time limit, log, statistics (host DLL 2)

As on the Pi, a kernel cannot take the machine down:

- **Faults:** an access violation, illegal instruction, division by zero or stack overflow inside a
  kernel ends the call with `NC_ST_FAULT`; WinUAE goes on. (A vectored exception handler sends the
  thread back to the start of the call; every call is guarded, at no measurable cost.)
- **Time limit:** a kernel that runs past its limit (1 s, `nc_set_timeout`) ends with
  `NC_ST_TIMEOUT`: a watchdog thread stops it, but only while it runs code of its own module, so
  that no lock of Windows or the C library stays taken. Each piece of `nc_run_mp` has the limit
  on its own. Its output may be half written.
- **Log:** `nc_printf` / `nc_log` write into a 16 KB ring in the host DLL (module version 2; rebuild
  modules made before). `NCLog` and `NCStatus` read it, and the DLL adds a line for every fault and
  time-out.
- **Fault details:** `NCLog FAULT` shows the Windows exception, the address, the PC's registers and
  a backtrace of module offsets; `nc addr2line build/x64/<module>.dbg.dll <offsets>` turns it into
  source lines (modules keep a copy with symbols). GCC for Windows x64 keeps no frame pointer chain
  (`rbp` points into the frame), so the DLL walks the frames with the module's unwind tables
  (`.pdata`, `RtlVirtualUnwind`) and stops at the first frame outside the module.
- **Statistics:** `NCLog STATS` and `NCStatus` say "WinUAE x64" (`NC_FEAT_X64` in the features),
  and show the share of time the emulation thread and the worker threads (on average) spend in
  kernels. `NC_GetInfo` still says FALSE.

Tested with `hello FAULT TIMEOUT LOG`: the fault ends with `0x84` (exception `C0000005`, address 4),
the time-outs of `nc_call`, `nc_run_mp` and `nc_start_mp` with `0x87` after 17 to 27 ms (limit 20 ms),
the log text is the Pi's to the byte. `hello LOG` gets the four frames of its nested fault
(`df3`, `df2`, `df1`, `deepfault`) and finds the bad pointer in `rcx` (`NCFaultInfo.x[1]` on the PC,
`x0` on the Pi). A stack overflow (a kernel recursing with 4 KB per level) ends with `0x84`
(exception `C00000FD`) on WinUAE's emulation thread and on the pool's threads, twice in a row on each
with correct calls in between: the thread gets its guard page back.

Results checked against the 68k: `mp3demo BENCH` (20 s, 1219 of 1.77 million samples differ by 1, as
on the Pi), `nbodydemo`'s step kernel (200 steps of 1500 bodies: positions within 3e-5 of the 68k's,
`nc_run` and `nc_run_mp` bit-identical), `bench` (all checks), `hello HEAP` (with `malloc` on the
heap). `cputest` reports its Mandelbrot as DIFFERENT under WinUAE: WinUAE's 68k FPU computes with
80 bits (76 202 300 iterations), while the PC gives exactly what the Pi's 68k and ARM give
(76 318 331).

## Clock

The emulation stands still while a kernel runs on the PC, and the Amiga's clocks see only part of
that time: a call of 1 s looked like 85 ms to timer.device, one of 200 ms like 20 to 114 ms
(`GetSysTime` the same). `nc_now_us` and `nc_now_ticks` (compiled into the program, `ncsupport.c`)
therefore read the PC's clock (`NC_GetStats`, 1 MHz) once `nc_open` found the x64 backend, so
times around native calls and frame rates are right. The clock is chosen again when the library is
opened or closed: take both ends of a measurement while the kernels are open. Programs that time
themselves with timer.device (`bench`, `cputest` use `ReadEClock`) still see the Amiga's time.

Measured with calls stopped by their time limit (microseconds):

| limit | PC clock | `nc_now_us` | `GetSysTime` |
|---:|---:|---:|---:|
| 50 ms | 55 097 | 55 088 | 48 898 |
| 200 ms | 219 549 | 219 553 | 21 269 |
| 1 s | 1 046 784 | 1 046 789 | 174 814 |

`nc_now_us` costs 1.1 us a call there (one library call through `uaenative.library`).

## Memory

Kernels reach all of the Amiga's RAM, chip RAM too. `nc_addr_ok` and `nc_addr_limits` (compiled into
the program, `ncsupport.c`) say so once the program has opened its kernels (`nc_open`) and the
library reports `NC_FEAT_X64`; programs built with an older SDK keep the Pi's rule (Fast RAM from
16 MB up), and say FALSE for chip RAM there.

`nc_heap` works as on the Pi: its lock holds between the pool's threads, and `malloc` & co. of the
SDK's `stdlib.h` use the heap chosen with `nc_heap_select` (before this SDK, x64 modules locked
nothing and took `malloc` memory from Windows). `nc_printf` is the Pi's formatter (`%d %i %u %x %X
%p %s %c`, no floating point).
