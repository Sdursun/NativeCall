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
  limit). Hypervisors 1–4 lived there, at `0x1_0000_0000`; hypervisor 5 takes the top 2 MB of Emu68's RAM
  from the device tree instead (section 25).

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

**Memory:** the resident part went to `0x1_0000_0000` (hypervisor 5: the top of RAM, section 25). It runs its own EL2 MMU: an identity map (Normal
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
- So cores 1 and 3 go through `boot.S` → `hv_secondary` (hypervisor base + 4), which sets up the EL2 MMU (same
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

## 25. Any RAM size: the hypervisor finds its place in the device tree (hypervisor 5)

**Problem:** the hypervisor was fixed at `0x1_0000_0000`. A Pi 4 with 1, 2 or 4 GB has no RAM there: black screen.

**What Emu68 does (`af63580`):**
- It learns its RAM only from the `reg` of the first `memory` / `memory@...` node (`mmu.c:243`, `devicetree.c:259`).
- It takes its own memory **from the top of the last block**: MMU pool, JIT (`mmu.c:268-290`), then the kernel's
  32 MB (`start.c:1319`). That top must be 2 MB aligned.
- The framebuffer comes from the block ending below 1 GB; if the last block is a different one, it is not touched.
- At boot the 32 MB from where the kernel was loaded are in use (`start.c:806-831`), so a fixed low address is not
  safe. The top of the first block depends on `gpu_mem`; a hole cannot be cut in the middle of a block.
- `linux,initrd-start` is read as 32 bits (`start.c:1134`).

**Solution:**
- `hv_fdt.c` runs in `boot.S`, MMU off, little-endian. It takes `HV_RESERVE` (2 MB) from the last block and writes
  one size cell of its `reg`. It refuses and writes nothing for: a bad header, no memory node, size 0, an unaligned
  top, a block that is too small, RAM outside 8 GB, or a region overlapping the DTB or the initrd.
- **When it refuses:** Emu68 starts as with the stock armstub (secondary cores see `hv_base == 0`). No native calls,
  programs take their 68k path, no black screen. With `DEBUG_LED=1` the reason shows on the LED: a long flash, then
  one short flash per status code.
- **The hypervisor is position independent:** `-fPIE -mcmodel=tiny`, symbols through `adr`. The Makefile links it
  at 0 and at 2 MB and compares the two. 8720 bytes, `0x59000` with bss; the linker checks that it fits in
  `HV_RESERVE`.
- **The EL2 map comes from the real RAM:** `gb_mask` (in `x3`). The old map assumed 8 GB, which allowed speculative
  accesses to RAM that is not there. The maintenance alias follows the same mask.
- `boot.S`: the `hv_base` word is at `0xd0` (in the page the firmware reserves with `/memreserve/`), stack at
  `0x70000`.
- On the A1200 the hypervisor lands at `0x7FE00000` and the 68k sees 2 MB less Fast RAM. `NC_HV_VERSION` 5;
  nothing changes for the 68k, the library is the same.

**QEMU** (`qemu/test_all.py` checks everything; `qemu/run.sh fdt` the parser alone, 16/16):

| Scenario | Machine | Expected and result |
|---|---|---|
| `base` | raspi3b, raspi4b | no hypervisor, status 0 |
| `hv` | raspi3b, raspi4b | loader + hypervisor, all tests; top of RAM 2 MB lower |
| `refuse` | raspi3b, raspi4b | status 0, top of RAM unchanged |
| `stub` | raspi4b | **the real armstub**, all cores from 0 at EL3; all tests |
| `stubrefuse` | raspi4b | the real armstub, initrd at the top of RAM: refused, status 0 |

- `nativecall-qemu:3` (`tools/qemu/Dockerfile.qemu9`): QEMU 9.2 `raspi4b` with three patches (`qemu9-patch.py`):
  the `wfe` trap; RAM in one node with two blocks, as the firmware writes it; initrd values as 32 bits. Booted
  stock, Emu68 reports the same RAM limits as on the A1200 (`0x3a9fffff`, `0x79dfffff`), so this is the A1200
  layout.
- **A bug the test caught:** the mask was passed in `x2`, which `hv_entry` overwrote. The 1 GB layout did not show
  it; in the Pi 4 layout the hypervisor was outside its own map. So `raspi3b` alone is not enough.
- The `stub` test does the firmware's job: files in RAM, `dtb_ptr32` and `kernel_entry32` filled in, `0xf0` clear.

**Not tried on hardware, and QEMU does not show:**
- The real Pi 4 firmware on other RAM sizes: where it puts the DTB and initrd, and whether `reg` has the expected
  form. On an overlap it refuses, so the bad case is "no native calls".
- Caches (QEMU does not model them). The DTB write and the copy happen with the MMU off.
- 1, 4, 8 GB without `total_mem`: parser tests only, not with Emu68. These layouts were written from memory; the
  only layout seen on hardware is 8 GB + `total_mem=2048`.

### 25.1 Hypervisor 5 on the A1200

- Pi powered off and on, normal boot. Total Fast RAM `1920991232` → `1918894080` (exactly 2 MB less, as expected).
- `hello LOOP=1000 FAULT MP ASYNC`, GCC and VBCC: 1000 calls 0 errors, copy ok, fault caught (ESR `0x96000006`,
  FAR 4), asynchronous job ok, `nc_run` 174.9 ms, `nc_run_mp` on 3 cores 66.1 ms (2.65x), no units missing.
- `bench` (GCC release build, first time on the A1200): all checks ok. Adler32 NEON 6.2x, CRC32 instruction 15.7x,
  matrices NEON 11.6x. Temperature 46–49 °C.
- No real test on a 1, 2 or 4 GB Pi yet.

## 26. Robustness: time limit, log, fault details, memory protection, job cancel (hypervisor 6 and 7, library 1.6)

### 26.1 What was missing

| Gap | Behaviour before | Solution |
|---|---|---|
| Endless loop | Core 0: the machine locks. Cores 1/3: the core and the job are lost, `nc_wait` never returns | EL2 timer (`CNTHP`, PPI 26) as an FIQ, armed only while native code runs; past the limit the call ends through the fault path with `NC_ST_TIMEOUT` (26.2) |
| No output from native code | Only ESR/FAR/ELR | A ring buffer in the hypervisor, `nc_log`/`nc_printf` on the native side, `NC_OP_LOGREAD` and the `NCLog` tool (26.4) |
| Fault details | ESR, FAR, ELR | x0–x30, SP and the frame pointer chain as module offsets; `nc addr2line` on the PC (26.4) |
| Memory protection | Native code at EL2 could write to all RAM, including Emu68's kernel and JIT and the hypervisor's own tables | Hypervisor code and page tables read-only, guard pages under the stacks, Emu68's private memory taken out of the native map (26.5) |
| Abandoned jobs | A program that exits or crashes without `nc_wait` leaves helpers writing to freed memory | `NC_OP_CANCEL`; the library cancels on unload and for dead tasks (26.8) |
| Other hardware | Only a Pi 4 with 8 GB was seen | 1/2/4 GB: hypervisor 5 is ready, hardware is needed. CM4 and Pi 400 have the same BCM2711. Pi 3A+ and Zero 2 W (BCM2837) need an armstub of their own: peripherals at `0x3F000000`, the old interrupt controller, no GIC. Not done |
| Emu68 versions | The hypervisor relies on Emu68's internals (D0/D1 in x19/x20, `CPUSHL` → `dc civac`, a PMU access on secondary cores, the idle `wfe` loop) | Most changes fail safe (no PING reply → 68k path; no helpers → one core). New Emu68 versions should be run in QEMU |

### 26.2 Time limit (hypervisor 6)

- **Why an FIQ:** Emu68 uses Group 1 IRQs of the GIC on core 0 (Amiga level 6, `vectors.c:2420`) and `CNTP` on
  core 3 for PPC. It never touches the EL2 timer (`CNTHP`) or FIQs. Taking IRQs to EL2 (`HCR_EL2.IMO`) would steal
  Emu68's interrupts too.
- **armstub (EL3, every core):** PPI 26 in Group 0, priority 0, enabled; `GICC_CTLR = 0x1ef` (FIQEn). These are
  Secure bits: Emu68's Non-secure writes of `GICD_CTLR = 1` / `GICC_CTLR = 1` cannot change them.
- **Hypervisor:** `HCR_EL2.FMO` (core 0 at boot, helpers in the PMU trap). `nc_call_native(fn, params, limit)` sets
  `CNTHP_CVAL = now + limit`, `CNTHP_CTL = 1`, then `DAIFClr #1` and runs the native code. On return FIQs are
  masked and the timer is off. An FIQ at EL2 → `hv_native_timeout`: timer off, ELR saved, return through the
  fault recovery path (`hv_native_unwind`) with `fault_flag = FAULT_TIMEOUT`. A late FIQ from EL1 is only
  switched off.
- **ABI:** `info[5]` = limit in µs (CALL, RUNMP, START; per piece for RUNMP/START), 0 = 1 s. `NC_ST_TIMEOUT` 0x87,
  info as for a fault (ESR 0, FAR 0, ELR). PING `info[7]` = `NC_FEAT_*`.
- **Self-test:** on every PING the hypervisor spins its own code for 200 µs with a 50 µs limit; if the FIQ comes,
  it reports `NC_FEAT_TIMEOUT`. Without an FIQ path (`enable_gic=0`, another armstub) that costs 200 µs, not the
  machine.
- **Old library:** 1.5 does not clear `info[5]`. What is left there (an ELR ≥ 16 MB, an ESR, 0 after PING) always
  means ≥ 16 s or the default: the limit gets looser, never tighter.
- **Library 1.6:** a limit per module (`NC_SetTimeout`, LVO −138, so programs cannot change each other's limit),
  `info[5]` written with every request, `NC_ST_TIMEOUT` recorded for `NC_LastFault`. SDK: `nc_set_timeout(k, us)`.
  `hello TIMEOUT`; kernel `spin` in `hello` (added at the end, older entry numbers unchanged).
- **Found in QEMU:** raspi4b's GIC is modelled without Security Extensions; Emu68's `gic_local_init` on core 0
  cleared FIQEn, and the timer arrived as an IRQ nobody took (`-trace gic_*`). The GIC-400 in the BCM2711 has
  the extensions: patch 4 in `tools/qemu/qemu9-patch.py`.
- **Limit of the limit:** after a time-out the kernel's output is half written and a lock it held (for example
  `nc_heap`'s) stays taken. The module's state is unknown: reload it or report the error.

### 26.3 Hypervisor 6 on the A1200

- `hello LOOP=1000 FAULT MP ASYNC TIMEOUT` (GCC and VBCC, and `HEAP`) passed; the self-test reported
  `NC_FEAT_TIMEOUT`.
  - `nc_call(spin)` with a 20 ms limit: `0x87` after 21.2–21.5 ms, ELR inside the module; then `add` = 42.
  - `nc_run_mp(spin)`: `0x87` after 20.5–21.0 ms. `nc_start_mp(spin)` + `nc_wait`: `0x87` after 20.2–20.4 ms.
  - The rest as with hypervisor 5: `nc_run` 175.1 ms, `nc_run_mp` 66.3 ms, ASYNC 87.6 ms.
- `hello TIME`: `nc_call(add)` 5.90 µs (5.4–6.5 before), `nc_run_mp` 12.6 µs, start + wait 15.3 µs: arming the
  limit has no measurable cost.
- The old library 1.5 on hypervisor 6: `LOOP=1000 FAULT MP ASYNC` with the same results.
- A `gic400.library` was open on the test machine (other software reaching the GIC-400 from the 68k). Being
  Non-secure, it cannot change the Group 0 setup; the self-test confirmed that.

### 26.4 Log and fault details (hypervisor 7, library 1.6)

- **Log:** a 16 KB ring in the hypervisor (`NC_LOG_SIZE`); positions count the bytes written since boot. Writers:
  - native code: `nc_printf` / `nc_log` in the SDK runtime (`narm_rt.c`; `%d %i %u %x %X %p %s %c`, `-`/`0`/width,
    `l`/`ll`/`z`). `TPIDR_EL2` → `hv_cpu` + `NC_CPU_ENV` (144) → `struct NCEnv` → `log_write` (hypervisor code,
    called directly from native code). On hypervisor 5/6 that word is 0 and nothing is written. On the 68k side
    the macros are empty.
  - the hypervisor: a line for every fault and time-out, `nc: fault on core N, ESR …, FAR …, module … +offset`.
  - `hv_log_write` copies the text to its stack first (a bad pointer faults before the lock is taken) and holds
    the lock with FIQs masked (a time-out cannot interrupt it).
- **`NC_OP_LOGREAD` (8), `NC_OP_FAULTINFO` (9).** `struct NCFaultInfo` (336 bytes, all 32 bit): status, core,
  module, counter, ESR/FAR/ELR/SP, x0–x30, a backtrace of 10 steps as module offsets (ELR, LR, then frame records
  while they are inside the stack, 8-aligned and ascending).
- **Saving the registers:** at fault or FIQ entry no register and no native stack can be trusted. x5 goes to
  `CNTHP_CVAL_EL2` first (a full 64-bit register, the time limit's own, switched off on every exit anyway), then
  x0–x30 and SP go to `hv_cpu.regs` through `TPIDR_EL2`. (`VTTBR_EL2` was considered: its VMID field does not hold
  64 bits.) `hv_cpu` is 768 bytes.
- **SDK:** kernels are built with `-g -fno-omit-frame-pointer` (the `.narm` size does not change, the
  information is in the `.elf`). `nc addr2line <module>.elf <offset>...` (modules are linked at 0: offset =
  address; return addresses minus 4). Library: `NC_ReadLog` (−144), `NC_GetFaultInfo` (−150), through a buffer
  above 16 MB (the caller's may be below). Tool `NCLog` (`FOLLOW`, `FAULT`). `hello LOG`, kernels `logtest` and
  `deepfault` (df1 → df2 → df3).
- **QEMU:** the backtrace `0x30 0x4c 0x6c 0xeb0` → `nc addr2line`: `df3 :167`, `df2 :172`, `df1 :179`,
  `deepfault :192`. The A1200 gave the same four offsets.
- **Lesson:** the QEMU `stub` scenario rebuilds the armstub with its own compiler. To test the binary that goes on
  the card, copy it into `build-qemu-stub/` and `touch` it; if a source is newer, make rebuilds it. Always compare
  the md5 afterwards.

### 26.5 Memory protection (hypervisor 7)

- The hypervisor's 2 MB are mapped with 4 KB pages (`l3_hv`): code read-only, page tables read-only (`.bss.pt`,
  updated through the maintenance alias), the lowest page of every stack unmapped (guard). All other RAM in 2 MB
  blocks.
- On the first request `restrict_native_map()` removes from the identity map every 2 MB block of RAM that EL1
  does not map 1:1: Emu68's kernel, JIT cache and MMU pool, GPU memory (`0x3AA00000`–`0x3FFFFFFF`), RAM above
  4 GB. A1200 and QEMU raspi4b: 1928 MB visible, 102 MB hidden.
- `hello PROTECT`: a write to code gives ESR `9600004f`, a read of Emu68's memory `96000006` (FAR `0x7fd00000`),
  a stack overflow `96000047` (FAR in the guard page). All `0x84`, the machine goes on.
- **`PiReboot`** (`sdk/tools/pireboot.c`): `PiReboot NOW` restarts the Pi with the BCM2711 watchdog, as Emu68's
  `pi_reset()` does (`PM_RSTS` partition 0, `PM_WDOG` 10, `PM_RSTC` full reset; from the 68k at `$F2000000`). The
  firmware reads the armstub again, so it replaces a power cycle. It needs no hypervisor and is of no use on a
  frozen machine. The Pi is back after about 15 s; the hypervisor's log is gone.

### 26.6 An instability that looked like hypervisor 7

With hypervisor 7, `bench` in a loop reset the Amiga within about 20 rounds on most boots. The Pi stayed up (the
hypervisor's log survived), there was no Guru. A day went into the hypervisor:

| armstub | boots: clean / crashed |
|---|---|
| hypervisor 6 | 3 / 0 (160 + 100 + 100 rounds) |
| hypervisor 6 padded to the size of 7 | 1 / 0 |
| hypervisor 7 (with and without protection, diagnostics, stack painting, ...) | about 2 / 10 |
| stage 2 for EL1 on all cores, hypervisor memory unmapped | 3 / 1, and no EL1 access was ever caught |

- `bench` causes no fault or time-out; the hot paths (`hv_lower_sync`, `nc_call_native`, `hv_entry`) were the same
  instruction for instruction as in hypervisor 6. `-fanalyzer` was clean.
- Results changed from boot to boot. **One clean boot proves nothing; an experiment needs three or four.**
- What turned it: Emu68's own log at the moment of the crash (`async_log` in `cmdline.txt`; the hypervisor
  searched RAM for Emu68's log buffer after the reset and copied its end into its own log, `HV_EMU68LOG`). The last
  lines were `[GIC] gic_local_disable()`, `[PS] CPU sending RESET`: no exception, no JIT error. **The 68k
  executed a RESET instruction**, so 68k memory or state was being corrupted.

### 26.7 The cause: modules did not contain their `.bss`

**The bug was in the SDK's `native/narm.ld`, not in the hypervisor**, since `--gc-sections` was added (releases
1.5.0 and 1.6.0 have it):

- With `.data : { *(.data*) *(.bss*) *(COMMON) }`, once only `.bss` input is left in the output section it becomes
  NOBITS, and `objcopy -O binary` leaves it out of the image. `bench.narm` was 1248 bytes (`0x4e0`), but
  `crc_table_ready` was at `0x4e0` and `crc_table` at `0x4f0`–`0x8f0`: **outside the image**. The library allocated
  the image size + 4095 and aligned to 4 KB; when less than 1040 bytes of that slack were left, the native
  `crc32_table` kernel wrote past the allocation, into exec's free list.
- Once the table was written its flag stayed 1 (RAM survives even a Pi reset); when that memory was reused and
  cleared by someone else, the table was written again. Rare, and dependent on addresses and history: hence the
  boot-to-boot differences, and hypervisor 6 looking clean (it had the same bug; luck).
- **How it was found:** `HV_RESETSPY` (a stage-2 trap on the GICC page; when Emu68 writes there in
  `gic_local_disable`, the hypervisor logs the 68k state). At the RESET: `D7 = 0x81000005` (`AN_MemCorrupt`), PC in
  ROM, and on the user stack `0x27100` (160000, the size `bench` passes to `FreeMem`). Then `readelf`: `.data`
  NOBITS, `__narm_end` beyond the file size.
- **A/B on one boot:** the fixed `bench` ran 200 rounds clean, the old `bench` crashed again.

**Fix:**
- `narm.ld`: `QUAD(0)` at the end of `.data` (the section becomes PROGBITS, `.bss` is in the image as zeros),
  `__narm_end` = end of the image. The linker's warning `section .data type changed to PROGBITS` is expected.
- `mk/nativecall.mk`: the build fails if the image is smaller than `__narm_end`.
- Library 1.6: 64 KB of cleared slack behind every module (`MODULE_SLACK`), so modules built with the old SDK are
  safe with the new library if their `.bss` is at most 64 KB.
- **Other projects:** every module built with the 1.5.0 or 1.6.0 SDK whose kernels have zero-initialised static
  or global variables is affected and must be rebuilt.

**Lesson:** for a rare crash, first find out what the 68k says at the moment it dies. The diagnostic code is still
in the source, switched off: `make HV_EXTRA="-DHV_DIAG -DHV_CANARY -DHV_RESETSPY -DHV_EMU68LOG -DHV_S2"`.

After the fix, over two boots on the A1200: `hello LOOP=1000 FAULT MP ASYNC TIMEOUT LOG PROTECT HEAP TIME` (GCC,
VBCC), the full test set 20 times, `bench` 200 + 150 rounds, `mandeldemo`. All clean.

### 26.8 Job cancel and cleaning up at exit

A job started with `nc_start_mp` kept running on the helpers when the program exited without `nc_wait`, writing
to freed memory, and its slot (one of four) stayed taken for good.

- **Hypervisor 7: `NC_OP_CANCEL` (10)**, arg0 = job. `JOB_CANCELLED` (0x100) is CASed into `hv_job.fault`; the
  existing path "after a fault, pieces are only counted" skips the remaining pieces, and core 0 counts them all at
  once. Running pieces finish (at most the time limit). `WAIT` → `NC_ST_CANCELLED` (0x88), result = the sum of the
  pieces that ran. No fault is recorded. An unknown job gives `0x86`.
- **Library 1.6:** `NC_CancelJob` (−156). `NC_UnloadModule` and expunge cancel jobs instead of waiting for them.
  `reap_jobs` cancels the jobs of tasks that are gone (a Workbench program, a crash) in `NC_StartMP` and
  `LibOpen`. On a hypervisor without cancel (`NC_ST_BADOP`) it falls back to waiting.
- **SDK:** `nc_cancel(k, &job)`. `ncsupport.c` registers an `atexit` handler on the first `nc_open`: if the
  program exits without `nc_close`, its kernel sets are closed (jobs cancelled, modules unloaded). The reason: CLI
  programs run in the Shell's task, so the library cannot tell from the task that the program ended. Programs
  without a C runtime: `-DNC_NO_ATEXIT`.
- **A1200:** `hello CANCEL`: an 87 ms job cancelled after 10 ms gives `0x88`, 2048 of 16384 units, and the cancel
  takes 1.0–1.5 ms. `nc_close` with a running job returns after 11 ms. `hello LEAK` six times (starts a job and
  exits without `nc_wait` or `nc_close`), then `hello ASYNC CANCEL`: all right, modules unloaded at exit.
- **Testing lesson:** if an old library with the same version number is still in memory, `OpenLibrary(path)`
  returns it, and calling a new LVO crashes the machine. `Avail FLUSH` or a restart before testing a new library.

## 27. Why `ncdemo`'s 68k path had become slow: compiler flags and Emu68's JIT

- **Symptom:** `ncdemo` built with GCC ran its 68k path at 24.8 fps; RESULTS.md had 44.6. The native path was
  unchanged (164–169 fps).
- **Not the hypervisor, library or environment:** the old 1.5.0 release binary gave 44.3 fps on the same boot.
  The compiler had changed: the base image's tag moves, and this build had GCC 16.2.1b instead of 16.2.0b.
- **Two things are expensive in Emu68's JIT:**
  1. float → int: with `-m68040` the compiler does what a real 68040 needs: save FPCR, set round-to-zero, convert,
     restore (about 85 ns per conversion). `FINTRZ` does not exist on the 68040 but does in Emu68, and is cheap.
  2. loops closed with `DBF`: Emu68 ends its translation unit there.
- **Fix (`sdk/mk/nativecall.mk`):** GCC `-O2 -m68040 -mtune=68060 -mhard-float -fno-branch-count-reg`, VBCC
  `-cpu=68040 -fpu=68060`. On a real 68040, 68040.library emulates `FINTRZ` (slow but right). Measurements:
  RESULTS.md, "68k compiler flags".
- **After it:** `ncdemo` GCC 68k 49.0 ↔ native 164.2 fps (3.4x), VBCC 68k 40.5 ↔ native 164.4 fps. `mandeldemo`
  did not change (double arithmetic, no conversions). `hello`, `bench`, `cputest`: same 68k times as before.
- **Lessons:** the "native is N times faster" ratio depends on the 68k build (6.7x → 3.4x from flags alone).
  Projects with their own 68k build can gain from the same flags. The compiler version should be pinned in the
  image (open).

## 28. Status: hypervisor 8, library 1.7, `NCStatus`

Users asked: is NativeCall active, how loaded are the cores, how hot is the Pi, which programs are calling?

- **Hypervisor 8:** per core in `hv_cpu`, `busy` (counter ticks spent in native code) and `calls`, added up in
  `do_call` and `run_pieces`. New request `NC_OP_STATS` (11) → `struct NCStats` (104 bytes): counter, frequency,
  temperature, jobs, faults, log position, 4 × `NCCoreStats` (busy, calls, `NC_CORE_NATIVE/HELPER`), hypervisor
  version, `NC_FEAT_*`.
- **Temperature:** the BCM2711's AVS monitor (`0xfd5d2200`, the formula of Linux's `bcm2711_thermal`:
  410.04 − 0.487 · value). It is read under `nc_call_native`, so a fault is caught: on a machine without the
  register the answer is "unknown" (0). QEMU: 0. A1200: 69.1 °C while `ENV:Emu68/Temperature` said 70.1.
- **Library 1.7:** `NC_GetStats` (−162), `NC_ListModules` (−168, `struct NCModuleInfo`: owner's name, kernels,
  calls, ARM time, jobs, faults, time limit). The owner's name (CLI command name or task name) is stored when the
  module is loaded.
- **`NCStatus`** (`sdk/tools/ncstatus.c`, GCC, MUI 3.8+): a state line, gauges for the four cores and the
  temperature, the list of programs (calls per second, share of ARM time), the hypervisor's log, "Show last
  fault". Says so if the library or hypervisor is missing; with an older hypervisor or library it takes the
  temperature from `ENV:`. Icon: `sdk/tools/mkicon.py` draws it (46x46, 32-colour GlowIcon, a selected image with
  a glow, and a 4-colour classic image).
- **`NCLog STATS`** (NCLog 1.1): the same in the Shell, from a one-second sample.
- **A1200:** during `mandeldemo` NEON MP, `NCLog STATS` shows three cores at 78–79% and 14 700 calls per second
  each; in the one-core phase core 0 is at 93%.
- **An expensive lesson (the A1200 rebooted):** the inline `MUI_NewObject(cl, Tag tags, ...)` in the MUI SDK's GCC
  header passes `&tags` as the tag list. When GCC 16 inlines the function, the other tags are not behind that
  address: broken objects, a crashed machine. Fix: `NO_INLINE_STDARG` and an own `MUI_NewObject` that copies the
  tags with `va_arg`. Also: libnix `printf` reads `%u` as 16 bit; use `%lu` with a cast.

## 29. New demos, `ncdemo` on all cores, a shared demo skeleton

- **Skeleton** (`sdk/examples/common/`): `demofw.c/.h` (screen, the modes 68k / native / native MP, keys, timing,
  summary; included at the end of a demo's `main.c`), `demopix.h` (pixel formats: kernels compute 0x00RRGGBB,
  `demo_store`/`demo_pack`/`demo_put` write in the screen's format), `cgx.h`, `bigstack.c`.
  - Screen: a 16-bit RTG mode first, then 32-bit, then 256 colours (a 3-3-2 palette, also without RTG). The RTG
    mode's pixel format is asked before the screen is opened (`GetCyberIDAttr`).
  - Arguments `WIDTH HEIGHT SECONDS AUTO MODE`; keys 1-3, N, Esc.
- **Demos:** `raydemo` (6 spheres, shadows, 3 bounces), `voxeldemo` (a ray per column over a height map that a
  kernel generates at start), `nbodydemo` (1500 bodies, all pairs, two buffers so that pieces only read the old
  state), `filterdemo` (bilinear rotate and scale, then a 3x3 filter: two kernels in a row), `mp3demo` (minimp3;
  plays through AHI with `nc_start_mp` decoding on an idle core; `BENCH` compares decoders). Figures: RESULTS.md.
  - `ncdemo` now has the three modes too. Its gain from several cores is small (raster 3.5 → 2.9 ms): every
    piece walks all triangles, and the rest of the frame is the copy to the screen.
- **Expensive lessons:**
  1. **The 68k version of a kernel runs on the caller's stack.** A Shell gives 4 KB; libnix `printf` + graphics +
     a kernel can exceed that, and minimp3 alone wants 16 KB. `bigstack.c` runs the demo's body in a child
     process with a 64 KB stack (`CreateNewProc`) and passes Ctrl-C on to it. Do not rely on the compilers'
     `__stack` variable: with VBCC memory was corrupted in tests (the exact cause was not isolated).
  2. **VBCC 0.9h and minimp3:** `-O2` ends in an internal compiler error, `-O1` produces 68k code that decodes
     wrong samples (`BENCH` showed it), `-O0` is right. `mp3demo/Makefile` sets `OPT68 := -O0` for VBCC (`OPT68`
     is new in the mk).
  3. **minimp3's NEON code is wrong on big-endian AArch64:** `MINIMP3_NO_SIMD`.
  4. **An emulator is not a stability test.** The WinUAE setup used here rebooted at random when demos ran one
     after another, known-good ones included; hours went into bugs that did not exist. Use it to see whether a
     program opens and draws correctly, and test stability on the real machine.

## Known limitations and open work

- Only the 8 GB Pi 4 has been tested on hardware. Hypervisor 5 handles other sizes (section 25), tested in
  QEMU only. No armstub for the Pi 3 or Zero 2 W.
- All hardware results come from one machine.
- After a time-out or a fault the module's state is unknown (half-written output, a held lock).
- The time limit needs the GIC variant of the armstub.
- The compiler versions in the SDK image are not pinned (section 27).
- `nc test` (a helper that compares 68k and native results), calling a kernel by name, and choosing 68k or
  native by measurement are ideas from a game port's report that are not done.
