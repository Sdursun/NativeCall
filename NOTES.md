# NativeCall: technical notes

The development log, condensed. Comments in the source refer to these sections (`NOTES.md 22`).

Emu68 source: https://github.com/michalsc/Emu68, commit `af63580`. Line numbers refer to that commit.
The Emu68 running on the test machine is `1.1.0-beta.1 git:0ba3a89`, 7 commits older; those commits do
not touch the files below.

**Design decision:** the Emu68 kernel is **not modified**. Instead of a patch and a new opcode, a small
EL2 program (hypervisor) replaces the **armstub** that the Pi firmware loads. See section 9.

## 1. Endianness: big-endian

| Evidence | Where |
|---|---|
| `#define EMU68_HOST_BIG_ENDIAN 1` | `include/config.h:40` |
| All C/C++ built with `-mbig-endian` | `CMakeLists.txt:278` |
| Link: `-Wl,-EB --format=elf64-bigaarch64` | `CMakeLists.txt:289` |
| `SCTLR_EL1.EE` and `E0E` set at boot | `src/aarch64/start.c:80-84` |

Native code is therefore built with `-mbig-endian`, and 68k data needs no conversion. AArch64
instructions are always little-endian; the linker (`-EB`) handles that. The Pi firmware and armstub run
little-endian, and Emu68 writes the spin-table words with `LE64()`.

## 2. Register mapping

| 68k | AArch64 | Source |
|---|---|---|
| D0-D7 | w19-w26 | `include/M68k.h:459-466` |
| A0-A4 | w13-w17 | `include/M68k.h:468-472` |
| A5-A7 | w27-w29 | `include/M68k.h:473-475` |
| PC | w18 | `include/M68k.h:457` |
| SR (with CCR) | `v19.h[5]` | `include/M68k.h:537-540` |
| FPSR / FPIAR / FPCR | `v19.s[0]` / `v19.s[1]` / `v19.h[4]` | `include/M68k.h:488-501` |
| JIT context pointer | `v20.d[1]` | `include/M68k.h:527-530` |
| CACR / USP / ISP / MSP | `v21.s[0..3]` | `include/M68k.h:507-525` |
| FP0-FP7 | d8-d15 | `include/M68k.h:479-486` |

68k registers are bound to ARM registers permanently. x4-x11 are JIT temporaries, x12 points at the
running translated code, and x12-x29 and v19-v26 are `-ffixed` for the compiler (`CMakeLists.txt:37-50`).

## 3. Address mapping: 68k address = EL1 virtual address

- **Lower half (TTBR0_EL1):** the 68k address space. Pi RAM blocks below `0xF2000000` are mapped
  phys = virt (`src/aarch64/start.c:1343-1368`). There is no base offset.
- Unmapped addresses (chip RAM, CIA, custom chips, Zorro) raise data aborts; the handler decodes the
  ARM load/store and sends it to the PiStorm bus (`src/aarch64/vectors.c:2246-2251`). **Native code must
  stay in Fast RAM.**
- **Upper half (TTBR1_EL1):** the kernel, physical memory at `0xffffff9000000000 + PA`.
- **Memory Emu68 takes for itself:** the very top of the last `/memory` block: MMU table pool, JIT, then
  the kernel moves itself there (`src/aarch64/mmu.c:268-290`, `start.c:1319-1322, 1434`).
- **On an 8 GB Pi 4 the RAM above 4 GB** is otherwise unused, never given to the 68k (`0xF2000000`
  limit). The hypervisor lives there, at `0x1_0000_0000`.

## 4. Opcodes (for the record; not used by the final design)

Line-A traps are used by Mac emulators. Line-F covers the FPU, MOVE16, CINV and CPUSH. Emu68 reserves no
opcode for itself; its channel to the 68k side is special MOVEC control registers `0x0E0-0x0EF` and
`0x1E0` (`src/M68k_LINE4.c:2220-2370`). `0x4AFA-0x4AFB` were the cleanest free opcodes, before the
decision not to patch Emu68 made this moot.

## 5. Interrupts and task switching

- 68k interrupts are checked **only between JIT blocks** (`src/ExecutionLoop.c:325`).
- **Core 2** reads the IPL from the PiStorm (`ps_housekeeper`, `start.c:482`). Core 1 is the optional
  async serial log, core 3 the optional PowerPC emulation.
- **Consequence:** during a native call on core 0, 68k interrupts (VBlank, CIA, keyboard, sound, serial)
  and AmigaOS multitasking **stop completely**. Latency equals the call's length. Rule: a call stays
  under about 1 ms.
- CPUSH and CINV end a JIT block (`src/M68k_LINEF.c:5436-5440`), so interrupts are checked on return.
- An unhandled ARM exception locks the machine (`src/aarch64/vectors.c:2396-2416`).

## 6. Cache

- Emu68 emulates the 68k I/D caches in software (`src/cache.c`).
- **68k CINV/CPUSH do not clean the ARM I-cache for 68k RAM.** ARM code written by the 68k needs
  `dc cvau` + `ic ivau` + `dsb ish` + `isb` on the ARM side: the hypervisor's `SYNC` operation.
- `CPUSHL DC,(An)` becomes `dsb sy; bic tmp,An,#mask; dc civac tmp; dsb sy` (`src/M68k_LINEF.c:5452-5462`).
- Normal JIT block generation uses PoU operations (`dc cvau`/`ic ivau`), which **`HCR_EL2.TPC` does not
  trap**. `dc civac` on core 0 is rare, so the trap costs practically nothing at run time.

## 7. Feature detection

Emu68 publishes its device tree to the 68k (`devicetree.resource`). The final detection needs none of
that: the 68k sends a `PING` call block, and a reply means native calls exist. Without the hypervisor,
`CPUSHL` is a harmless cache flush and the status stays 0.

## 8. Building Emu68 unmodified

```sh
docker build -t emu68-build:ubuntu24 Emu68/docker/ubuntu
docker run --rm -v <Emu68>:/src:ro -v emu68-orig-build:/build -w /build emu68-build:ubuntu24 bash -c '
  cmake /src -GNinja -DCMAKE_INSTALL_PREFIX=/build/install -DTARGET=raspi64 -DVARIANT=pistorm \
        -DCMAKE_TOOLCHAIN_FILE=/src/toolchains/aarch64-linux-gnu.cmake
  ninja && ninja install'
```

**Windows trap:** with `core.autocrlf=true` a clone gets CRLF and the Linux build breaks. Set
`core.autocrlf=false` and check out again.

## 9. Architecture: armstub + EL2 hypervisor

**Why it is possible:**

1. `armstub=file` in `config.txt` makes the firmware load our program at physical 0 and start the cores
   there at EL3 (official source: raspberrypi/tools `armstubs/armstub8.S`, BSD-3). Layout expected by the
   firmware: `spin_cpu0..3` at `0xd8/0xe0/0xe8/0xf0`, `stub_magic 0x5afe570b` at `0xf0`, `dtb_ptr32` at
   `0xf8`, `kernel_entry32` at `0xfc`.
2. Emu68 can start at EL1: `_start` checks CurrentEL, and at EL1 it goes straight to `continue_boot`
   and never touches EL2 (`start.c:73-170`). The stub does the few settings of `leave_EL2` itself.
3. Secondary cores start through the spin table; they must enter Emu68 at EL2, because
   `_sec_leave_EL2` sets up their EL1 MMU (`start.c:341-400`). See section 22 for how the hypervisor
   gets them anyway.
4. `HCR_EL2.TPC` (bit 23) sends EL1's `DC CIVAC/CVAC/IVAC` to EL2. `CPUSHL DC,(An)` on the 68k becomes
   exactly that.

**Memory:** the resident part goes to `0x1_0000_0000`. It runs its own EL2 MMU: an identity map (Normal
WB, inner shareable, as EL1, so caches stay coherent), `SCTLR_EL2.EE = 1`, no WXN, `CPTR_EL2.TFP = 0`.
The low 16 MB are not mapped, so native access to chip RAM faults. Incoming addresses are translated
with `AT S1E1R` + `PAR_EL1`.

**Trap handler:** each trapped DC operation is done on the physical address (so Emu68 sees no
difference); if the doorbell registers match (section 14), the call block is processed as well.

**Call block** (64 bytes, 64-byte aligned, Fast RAM, big-endian): see `sdk/include/nativecall/nc_abi.h`.

**Native code** runs with x0-x30, q0-q31, FPCR and FPSR saved, follows AAPCS64 and is built with
`-ffixed-x18`. A function must be one of the entries of a validated module.

**Rollback:** remove the `armstub=` line. The Emu68 file is never touched.

## 10. First implementation

Armstub, native SDK, 68k library and `hello` built; in WinUAE (68060, OS 3.2) the program took the 68k
fallback correctly, both without Fast RAM above 16 MB and with it (`status 0x00`: `CPUSHL` ran, no
hypervisor answered, no crash). The toolchain is the same `aarch64-linux-gnu-gcc-14` as Emu68's, not
`aarch64-none-elf-gcc`. There is no big-endian libgcc, so native code avoids what needs it.

## 11. First A1200 measurements (armstub off)

Pi 4 8 GB (revision `d03114`), 1.5 GHz. 68k Fast RAM: `0x01000020-0x08000000`,
`0x08000020-0x3aa00000`, `0x41000020-0x79e00000`: all above 16 MB and inside the EL2 map. `hello`
without the armstub: `status 0x00`, `42 (68k)`, so `CPUSHL` is harmless on real Emu68.

## 12. First boot failed: two bugs, reproduced in QEMU

With the armstub, the A1200 stayed on a black screen.

**Bug 1, Emu68's own cache flush was skipped.** Before moving itself up, Emu68 flushes its whole kernel
image with `dc civac` (`start.c:1440`) and reads the copy uncached. The kernel is at physical `0x0` then.
The hypervisor skipped those flushes: the low 16 MB were not in its map, and it used 0 as its failure
value. Fix: cache maintenance through a separate EL2 alias covering all RAM (`MAINT_ALIAS + PA`),
failure value `PA_INVALID`. The map native code sees did not change.

**Bug 2, staying at EL2 after a native fault.** A native fault is a second EL2 exception and overwrote
the outer trap's `SPSR_EL2`; the final `eret` then returned to EL2. Fix: `hv_lower_sync` saves `SPSR_EL2`
in the trap frame. Found through the QEMU `FAULT` test, not the cause of the black screen.

**QEMU test** (`qemu/`): since then every hypervisor change goes through QEMU first. It does not cover
the Pi 4 firmware, the GIC, or RAM above 4 GB; the `DEBUG_LED=1` build covers those by boot progress on
the ACT LED.

## 13. First native call on the A1200

```
hello              -> 42 (native)
hello LOOP=1000 FAULT -> 1000 calls (native), 0 errors; FAULT: status 0x84, ESR 0x96000006, FAR 0x00000004
hello LOOP=200000  -> 200000 calls (native), 0 errors
```

- The system stays responsive; a native crash (`FAR 0x4`) is caught.
- About 541 000 traps at boot are Emu68's kernel flush. At run time the count rises only with native
  requests: Emu68's normal JIT path does not trap.
- RAM above 4 GB works despite `total_mem=2048`.

## 14. Spontaneous 68k resets: phantom requests

**Symptom:** the Amiga reset without a Guru right after files were copied over the network. The
hypervisor's call counter had risen by 161 with nobody making native calls.

**Cause:** on every `dc civac` the hypervisor looked at the first word at the 64-byte aligned address
and treated `'NCAL'` as a call block, writing to it. Our own programs contain that constant. When the
network driver flushed DMA buffers where it happened to fall on a 64-byte boundary, the hypervisor wrote
into them and corrupted data.

**Fix:** a request is recognized by registers (the "doorbell"): D0 = `'NCAL'`, D1 = `~'NCAL'` just before
`CPUSHL` (`w19`/`w20` under Emu68). On all other traps the hypervisor does not touch memory. QEMU test
`SPOOF` covers this.

**Open point (theoretical, not observed):** the maintenance alias and native map mark everything Normal
WB, while Emu68 marks some regions differently; speculative cache fills during the short time at EL2 are
possible. The alias could be made Normal Non-cacheable if needed.

## 15. ADRP in native modules

**Symptom:** NEON Adler32 half wrong, table CRC32 returned 0.

**Cause:** GCC's default `small` code model reaches static data with `adrp` + `add :lo12:`. ADRP works on
4 KB pages, so the code is only right when loaded with the same offset modulo 4 KB as it was linked at.
Modules were aligned to 16 bytes. The independence check linked at 0 and `0x10000`, both 4 KB aligned, and
did not see it. The table CRC wrote its table before the module and corrupted Amiga memory.

**Fix:** the SDK builds with `-mcmodel=tiny` (byte-exact `adr`, module at most 1 MB), the check links a
second time at the unaligned `0x10010`, and modules are still loaded 4 KB aligned.

## 16. SDK and nativecall.library

Decisions: 68k side **VBCC and GCC**, a **shared library** rather than a static one, building in
**Docker**.

- One image `nativecall-sdk:1`: `ghcr.io/rondoval/amiga-build-container:gcc-v16.2` (m68k GCC 16.2, sfdc,
  fd2pragma, vasm) + VBCC 0.9h P3 and vlink 0.18a + NDK 3.2 from Aminet + `gcc-14-aarch64-linux-gnu`.
- `nativecall.library`: VBCC, hand-written romtag; GCC and VBCC headers generated from
  `sdk/sfd/nativecall_lib.sfd`. One call block; a request runs under `Forbid()`, so tasks take turns.
- Kernel system: `nativecall/kernel.h` (`NC_KERNEL`, `NC_DECLARE`, `NC_MODULE`, an X-macro list): one source
  becomes both the `.narm` and the 68k fallback.
- **GCC trap:** `-m68040` does not enable the FPU with this compiler (Mandelbrot 1966 ms and a different
  result); `-mhard-float` gives 19 ms and the right result.

Tested on the A1200 (both compilers, native; removing the `.narm` falls back to 68k), in WinUAE (68060, no
armstub, with and without the library; `Avail FLUSH` expunges cleanly) and in QEMU.

## 17. Game demo and library 1.1

`sdk/examples/ncdemo`: software 3D torus, 68k ↔ native live. `NC_Run` remembers its piece size per kernel.
An 8-bit RTG screen on Emu68's VideoCore card is really a 16-bit bitmap, and `WriteChunkyPixels` converts
every frame (24.7 ms), so the demo reads the pixel format through cybergraphics and converts in a kernel.

## 18. Report from a game port: fixes, single file, run times

- **GOT error:** `kernel.h` declared libc prototypes with default visibility, so `-fPIE -fno-plt` called
  `memcpy` through a GOT and the link stopped. Prototypes are now hidden; `hello` has a `copy` kernel as a
  regression test.
- `nativecall.mk` did not rebuild on header changes; all objects now depend on the headers.
- Added `nc_addr_ok()`, `NC_CHECK_SIZE` / `NC_CHECK_OFFSET`, `nc make narm`, `nativecall/nc_math.h`.
- **Single file (library 1.2):** `NC_LoadModuleMem`, `tools/narm2c.py` (module + CRC32 build ID as C data),
  `nc_open` order `ENV:NativeCall/ModuleDir`, embedded copy, `PROGDIR:` (a CRC mismatch gives
  `NC_ST_MISMATCH` and 68k).
- **Run times (hypervisor 2, library 1.2):** `CALL` replies carry CNTPCT ticks and CNTFRQ. Per-kernel
  table, `NC_GetKernelStats`, `nc_stats()`. `NC_Run` adjusts by measured native time; with hypervisor 1
  it falls back to the EClock.

## 19. A1200: library 1.2, hypervisor 2

Embedded module, `copy` and `FAULT` correct with both compilers. Build ID cases all right: no `.narm`
(embedded, native), right `.narm` (native), wrong `.narm` (`NC_ST_MISMATCH`, 68k), ENV override (native).
`ncdemo` 44.6 against 169.3 fps. **Open:** `raster`'s longest call was 2.5 ms, breaking the 1 ms rule.

## 20. Library 1.3: `NC_Run` learns the piece size

**Problem:** `raster` draws top to bottom. Empty top lines are cheap, so the piece grew and then hit the
busy middle. Shrinking quickly after a long piece was not enough (still 2534 µs). A bug in that attempt:
pieces measuring 0 µs counted as "unknown" and never grew.

**Solution:**
- First full run: grow while short (< 250 µs).
- Later runs: start at the learned size; within a call only shrink: after a piece over 750 µs, size × 500
  / time.
- At the end: double if all pieces were < 250 µs and several were needed; otherwise keep the shrunk size.

Result on the A1200: `raster` longest **752 µs**, fps unchanged.

**Unexplained observation:** during that session the `LIBS:` assign disappeared on the A1200 (`Assign LIBS:
EXISTS` said "not assigned"). No command removed it, repeating the same tests did not reproduce it, and it
stayed intact in all later tests. The cause is unknown. It was restored by hand; a single multi-target
`Assign LIBS: a b c d` silently did nothing there, one `ADD` at a time worked.

## 21. `mandeldemo`

Mandelbrot zoom, switching live between 68k, native double and NEON. The picture depends only on time, so
all modes show the same image and differ only in frames. Each row is `MD_SEGS` = 4 pieces, to keep a call
under 1 ms on deep rows. The on-screen comparison uses iterations per second, because modes run at
different depths. Results: RESULTS.md.

## 22. Several cores: hypervisor 3, library 1.4

**Emu68's cores** (`src/aarch64/start.c`, `secondary_boot`): 0 = JIT; 1 = serial writer with `async_log`,
idle otherwise; 2 = PiStorm housekeeper; 3 = PowerPC with `ppc_enable`, idle otherwise. Idle ones sit in
`while(1) wfe`.

**Capturing a core without touching the kernel:**
- Emu68 sets up the secondary EL1 MMU at EL2 (`_sec_leave_EL2`), so these cores must enter at EL2.
- It writes `HCR_EL2` outright (`TWE` cannot be set in advance), but ORs into `MDCR_EL2` and never touches
  `VBAR_EL2`.
- So cores 1 and 3 go through `boot.S` → `hv_secondary` (`HV_BASE + 4`), which sets up the EL2 MMU (same
  tables; nothing is read from memory before the MMU is on), stack, `TPIDR_EL2`, `VBAR_EL2`, `CPTR_EL2` and
  `MDCR_EL2.TPM`. Then Emu68's own code runs.
- `secondary_boot`'s first PMU access (`PMCR_EL0`) traps. The hypervisor sets `HCR_EL2.TWE`, clears `TPM`,
  enables the event stream (about 300 µs) and re-runs the instruction.
- From then on each idle `wfe` lands in the hypervisor (`helper_sync`): take pieces if there is work,
  otherwise `wfe` at EL2 and return. Core 2 and unexpected addresses go straight to Emu68 as before.

**`NC_OP_RUNMP`:** like CALL plus `[from, to)`, chunk and params size (up to 256). Core 0 copies the params,
posts the job (`sev`) and takes pieces too; helpers take from the same atomic counter. A helper that does
not come is not waited for, one that took a piece is (`job.busy`). Each core works on its own params copy,
results are added up, and the first fault is reported. Fault recovery is per core (`TPIDR_EL2`).

**Library 1.4:** `NC_RunMP` (LVO -108), `NC_Cores` (-114); `nc_run_mp`, `nc_cores`. `NC_UnloadModule` now
clears the module's kernel records, so statistics no longer leak into the next program.

**QEMU:** stock QEMU 8.2 never traps WFE. `tools/qemu/Dockerfile.wfe` + `wfe-trap.py` build a patched one
(`nativecall-qemu:2`). It needs `thread=single` (multi-threaded TCG skips the WFE helper, and forcing it
trips an assertion), so vCPUs take turns of about 100 ms: this verifies the protocol, not parallel speed.

**`async_log` and `ppc_enable`** (from the source; **not tested**): with `async_log`, `q_pop` waits with
`wfe` while the queue is empty (`src/raspi/support_rpi.c:48`), so core 1 helps whenever it is not writing.
With `ppc_enable`, the PPC JIT emits `wfe` only for an endless `b .` (`src/PPC_Translator.cpp:1680`), so
core 3 helps only while PowerPC idles. Work stealing means no waiting either way. `nc_cores` still counts
3.

## 23. Requests from a game port: clock, addresses, libc; asynchronous jobs (hypervisor 4, library 1.5)

**SDK only:**
- `nc_now_us()` / `nc_now_ticks()`: Emu68 is recognized by `devicetree.resource`; the counter is read through
  MOVEC `0x0e0`-`0x0e2` (`src/M68k_LINE4.c:2220`) in a small `Supervisor()` routine. Otherwise timer.device.
  2.0 µs against about 20 µs for timer.device on a PiStorm.
- `nc_addr_ok` / `nc_addr_limits`: the regions are read once, then only compared.
- `sdk/native/include/string.h`, `stdlib.h`; `nativecall/nc_heap.h` (offset based, first fit, address-ordered
  free list, merging, 16-byte alignment, atomic lock natively).
- `-mno-outline-atomics`, `-ffunction-sections -fdata-sections` + `--gc-sections`: unused runtime stays out of
  modules (`bench` 2496 → 1248 bytes).
- The base cost of a call is about 5.5 µs: about 4 µs for the request itself (Forbid, Supervisor, CPUSHL, EL2
  trap) and about 1.5 µs for module checks and the FPU save. The port's estimate of 15 µs came from
  subtracting a 20 µs clock.

**Hypervisor 4:** RUNMP and 4 async slots share `struct hv_job`.
- `NC_OP_START`: puts the job in a slot, wakes the helpers with `sev`, returns.
- Each finished (after a fault: skipped) piece counts down `pending`. The core reaching 0 writes the result
  into the 68k's `NCJobStatus`, then `done = 1` after a `dmb`.
- `NC_OP_WAIT`: if unfinished, core 0 takes pieces for up to 0.5 ms and returns `NC_ST_RUNNING`. If finished:
  wait for `busy == 0`, `dmb`, result, slot free.
- Helpers join the synchronous job first (core 0 waits for it with interrupts off), then the slots.

**Library 1.5:** `NC_StartMP` (-120), `NC_JobDone` (-126), `NC_WaitJob` (-132); `nc_start_mp`, `nc_done`,
`nc_wait`. `NC_UnloadModule` and expunge wait for the module's jobs. Without hypervisor 4 or with all slots
taken, the job runs at once.

**A1200:** with hypervisor 3 `ran at once`, correct. With hypervisor 4: only cores 1 and 3 ran the job, 87.9
ms against 174.9 ms on one core, and the 68k kept running. All older tests unchanged.

**Heat:** `mandeldemo` on 3 cores gave 25.2 and 24.0 fps in a row (27.1 earlier) while one core stayed at
10.4. `ENV:Emu68/Temperature` showed 80.3 to 83.7 °C, `TemperatureMax` 85.0 °C. The drop is thermal, not from
hypervisor 4.

**Reported by the game port, cause unknown:** one lockup while its 68k-only mode was running. The hypervisor
is active in every mode (all of Emu68's `dc civac` pass through it, and cores 1 and 3 are in it), so it cannot
be excluded; separating it needs a test with the `armstub=` line disabled. It did not recur.

## 24. `nc_math`: sin, cos, exp; a GPU-limited program

`nc_sin`, `nc_cos`, `nc_exp` (and `f` versions): plain C on both sides, because modules have no libm. Against
libm on a PC: `sin`/`cos` within 6e-12 for |x| up to 20 000, `exp` within 3e-16 relative.

**Trap:** the first version used `long long` conversions for range reduction. On the 68k those are slow library
calls under GCC: the kernel took 618 µs instead of 82 µs. They now use 32-bit conversions where the value fits.

Boing Ocean (ogles2.library example) with its CPU work as a kernel: 55.3 → 55.8 fps. The GPU sets the frame
rate; see RESULTS.md.

## Known limitations and open work

- Only the Pi 4 with 8 GB is supported and tested: the hypervisor's address is above 4 GB. Other sizes need
  either a separate build plus `total_mem` or detection through the DTB.
- All hardware results come from one machine.
- An endless loop in a kernel locks the machine (core 0) or that helper.
- There is no stack guard (64 KB per core).
- If a program crashes during an asynchronous job, a helper may write to freed memory.
- `ncdemo` does not use `nc_run_mp` yet.
