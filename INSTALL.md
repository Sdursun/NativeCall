# NativeCall: installation and rollback

NativeCall lets 68k programs call native AArch64 functions **without touching the Emu68 kernel**. A
small EL2 program (the hypervisor) replaces the **armstub** that the Pi firmware loads (NOTES.md 9).
Installing it means one file and one line in `config.txt`.

The binaries named below are in `binaries/` of a release zip, or in `dist/` after `./build.sh`. To run kernels
on the PC under WinUAE instead, see "WinUAE" below.

## Requirements

- PiStorm32-lite (or PiStorm32) + **Raspberry Pi 4**. The hypervisor takes the top 2 MB of the RAM
  that Emu68 would use (it reads and shortens the device tree), so the 68k sees 2 MB less Fast RAM.
  If the device tree does not look as expected, the armstub starts Emu68 like the stock one, without
  native calls (no black screen). Only the 8 GB model has been tested on hardware; 1, 2 and 4 GB
  layouts in QEMU (NOTES.md 25).
  - Hypervisor 4 and older (release 1.5.0) need the 8 GB model: they sit at `0x1_0000_0000`.
- Emu68 1.1 (tested: `1.1.0-beta.1 git:0ba3a89`, raspi64/pistorm). The Emu68 file does not change.
- `config.txt` must not have `enable_gic=0` (the Pi 4 default uses the GIC). If it does, build the
  stub with `make -C hypervisor GIC=0`; that stub has no time limit for native code.
- A limit such as `total_mem=2048` is no problem (tested): the hypervisor's 2 MB are removed from the
  device tree, so no other software uses them.
- `async_log` and `ppc_enable` in `CMDLINE.TXT` should not stop anything: according to the Emu68
  source those cores still help while they are idle. This has not been tested.
- A heatsink or fan is recommended: with three cores at full load a Pi 4 passes 80 °C and lowers its
  clock (NOTES.md 23).
- `NCStatus` needs MUI 3.8 or newer. Nothing else does.

## Installation

1. **Back up.** Copy `config.txt` from the FAT partition of the SD card.
   - From the Amiga: `Mount SYS:Storage/DOSDrivers/MountFAT` gives `EMU68BOOT:`, then
     `Copy EMU68BOOT:CONFIG.TXT TO EMU68BOOT:CONFIG.TXT.pre-nativecall CLONE`
   - From a PC: insert the card and copy `CONFIG.TXT` somewhere.
2. Copy `nativecall-armstub.bin` to the root of the FAT partition (`EMU68BOOT:` or the root of the
   card on a PC).
3. In `config.txt`, add below the `arm_64bit=1` line:
   ```
   # nativecall: EL2 hypervisor armstub (remove this line to go back to the firmware default)
   armstub=nativecall-armstub.bin
   ```
4. **Power the Amiga off and on.** The armstub is loaded only when the Pi starts; a software reset of
   Emu68 is not enough. (On a PiStorm32, Ctrl-Amiga-Amiga restarts the Pi too, which also works.)
5. Install `nativecall.library` and verify: see the sections below.

**Updating from an older release:** replace `nativecall-armstub.bin` and `nativecall.library`, then restart
the Pi (power off and on, or `PiReboot NOW`, below). Rebuild your own modules with the new SDK if their kernels
have zero-initialised static or global variables (NOTES.md 26.7).

## WinUAE (kernels on the PC)

No armstub is involved; the kernels run on the PC's processor (sdk/x64/README.md).

1. 64-bit WinUAE (`winuae64.exe`). Tested: WinUAE 6.1 beta 22 and 23.
2. Copy `WinUAE/nativecall-windows-x86-64.dll` to WinUAE's `plugins` folder (next to `winuae64.exe`,
   `plugins\`). WinUAE loads it only when an Amiga program asks for it.
3. Settings > Host > Miscellaneous > **Allow native code** (`native_code=true` in the configuration).
   This gives the Amiga `uaenative.library`; without it everything runs as 68k.
4. Copy `nativecall.library` 1.8 to `LIBS:` (below).
5. `NCLog STATS` says `WinUAE x64` when it works.

Programs built with this SDK carry their x64 module; v1.7.0 programs need their `<prefix>.nx64` next to
them. To go back, turn native code off or remove the DLL: programs run as 68k.

To replace the DLL, quit WinUAE first: Windows keeps a loaded DLL locked.

## Rollback

Any one of these is enough; nothing else is needed since the Emu68 file was not touched:

- Put a `#` in front of the `armstub=nativecall-armstub.bin` line in `config.txt` (or delete it), or
  restore the backup: `Copy EMU68BOOT:CONFIG.TXT.pre-nativecall TO EMU68BOOT:CONFIG.TXT`.
- If the Amiga does not start (black screen): put the card in a PC and comment out the same line.

Then power the Amiga off and on. Programs using nativecall see `NC_Available() = FALSE` and switch to
their 68k versions (tested in WinUAE and on Emu68 without the armstub).

### Boot diagnostics (LED)

`make -C hypervisor DEBUG_LED=1` builds the same stub with a boot trace on the Pi's green ACT LED
(copy it under the same name):

| LED | Meaning |
|---|---|
| 1 long flash (1 s) | the stub runs, about to read the device tree |
| 2 short flashes | the hypervisor was copied to the top of RAM and runs from there |
| 3 short flashes | EL2 MMU on |
| off | Emu68 is starting |
| a second long flash, then N short | the device tree was refused (N = `HV_FDT_*` status in `hypervisor/hv_fdt.h`); Emu68 starts without the hypervisor |

It adds about 4 seconds to the boot; use the normal `nativecall-armstub.bin` in everyday use.

## nativecall.library

Programs make native calls through `nativecall.library`:

```
Copy Libs/nativecall.library LIBS:
```

The library is harmless on systems without the armstub: programs switch to their 68k versions. If the
library is not installed at all, programs run too (again as 68k).

After replacing a library that was already in use, run `Avail FLUSH` or restart: an old copy with the
same name stays in memory otherwise.

## Tools

| File | Where | What |
|---|---|---|
| `C/NCLog` | `C:` | Prints the hypervisor's log (`nc_printf` of kernels, faults, time-outs); `NCLog FOLLOW` keeps printing; `NCLog FAULT` shows registers and a backtrace of the last fault; `NCLog STATS` shows versions, load per ARM core, temperature and the programs with native modules. |
| `C/PiReboot` | `C:` | `PiReboot NOW` restarts the Pi through its watchdog, so a new armstub is loaded without cutting power. It does not need the hypervisor; it cannot help when the machine is frozen. Save your work first: it is a hard reset. |
| `Tools/NCStatus` (+ `.info`) | anywhere | A MUI window with what `NCLog STATS` prints, updated every second, and the log. Start it from Workbench or the Shell. |

**Versions:**

| | |
|---|---|
| `nativecall.library` 1.8 | Without the hypervisor, kernels on the PC under WinUAE (`uaenative.library` + `nativecall-windows-x86-64.dll`); `NC_FEAT_X64`. With the hypervisor the same as 1.7. This release. |
| `nativecall.library` 1.7 | `NC_GetStats`, `NC_ListModules`: what `NCStatus` and `NCLog STATS` show (with hypervisor 8). |
| armstub, hypervisor 8 | Native time and calls per core, SoC temperature (`NC_OP_STATS`). Current since release 1.7.0. Tested in QEMU and on the A1200 (NOTES.md 28). |
| `nativecall.library` 1.6 | `NC_SetTimeout`, `NC_ReadLog`, `NC_GetFaultInfo`, `NC_CancelJob`; unloading a module cancels its jobs; 64 KB of slack behind a module. |
| armstub, hypervisor 7 | Log, fault details, memory protection, job cancel (NOTES.md 26.4 to 26.8). |
| armstub, hypervisor 6 | Time limit for native code (NOTES.md 26.2). |
| `nativecall.library` 1.5 | `NC_StartMP` / `NC_JobDone` / `NC_WaitJob`: jobs on the helper cores while the 68k goes on (with hypervisor 4; otherwise the job runs at once). |
| armstub, hypervisor 5 | Placed from the device tree (any RAM size), no change for the 68k. Tested in QEMU and on the A1200 (NOTES.md 25). |
| armstub, hypervisor 4 | Asynchronous jobs (`NC_OP_START` / `NC_OP_WAIT`, 4 slots). Tested in QEMU and on the A1200 (NOTES.md 23). |
| `nativecall.library` 1.4 | `NC_RunMP` / `NC_Cores`: work is shared with the ARM cores Emu68 leaves idle (with hypervisor 3; otherwise `NC_Run`). |
| armstub, hypervisor 3 | Uses cores 1 and 3 for `NC_RunMP` (core 2 is Emu68's housekeeper and is left alone). |
| `nativecall.library` 1.3 | `NC_Run` learns the piece size per kernel (NOTES.md 20). |
| `nativecall.library` 1.2 | Modules embedded in the program (one file), build ID check, native run time per kernel (`NC_GetKernelStats`). |
| armstub, hypervisor 2 | Reports the run time of each native call. |

Library and hypervisor versions work with each other in any combination; features simply fall back.

## Verification (SDK examples)

`gcc/hello` or `vbcc/hello` (the module is embedded, no `.narm` file is needed):

```
1> hello LOOP=1000 FAULT
1000 calls (native), 0 errors
copy: ok
FAULT: result 0xffffffff, status 0x84, ESR 0x96000006, FAR 0x00000004
```

- `Native calls not available: 68k version`: no library, the armstub is not loaded (`config.txt` line,
  file name; power the Pi off and on), or the CPU is not a 68040/68060.
- The `FAULT` test giving `status 0x84` is correct: the deliberate bad read in the kernel was caught
  and the machine goes on running.

Several cores with hypervisor 3 or newer:

```
1> hello MP
ARM cores for nc_run_mp: 3
nc_run     result 16384, status 0x1, units per core 0-3: 16384 0 0 0, missing 0, ...
nc_run_mp  result 16384, status 0x1, units per core 0-3: 6142 5124 0 5118, missing 0, ...
```

- `ARM cores ... 1`: the armstub is not hypervisor 3 or newer. Core 2 is never used (Emu68's PiStorm
  housekeeper).
- `missing` must always be 0.

Asynchronous job with hypervisor 4 or newer (`hello ASYNC`): `started`, units only on cores 1 and 3
(`0 8192 0 8192`), `missing 0`. With hypervisor 3 `ran at once` is normal.

The newer features (hypervisor 6 to 8, library 1.6 or newer):

```
1> hello TIMEOUT LOG PROTECT CANCEL
1> NCLog STATS
Hypervisor 8, nativecall.library 1.8, time limit works
Temperature 69.1 C
Core 0:   0 % native, 0 calls in the last second (68k)
Core 1:   0 % native, 0 calls in the last second (helper)
Core 2: used by Emu68
Core 3:   0 % native, 0 calls in the last second (helper)
```

- `TIMEOUT`: three times `status 0x87` after about 20 ms, and `add` still gives 42 afterwards.
- `PROTECT`: three times `status 0x84`.
- `time limit works` must appear in `NCLog STATS`. If it says otherwise, an endless loop in a kernel still
  locks the machine (stub built with `GIC=0`, or `enable_gic=0`).

`cputest`, `bench`, `ncdemo`, `mandeldemo`, `raydemo`, `voxeldemo`, `nbodydemo`, `filterdemo` and `mp3demo`
work the same way (RESULTS.md). The graphics demos take `WIDTH HEIGHT SECONDS AUTO MODE`; keys `1`-`3` (`1`-`4`
in `mandeldemo`) or `N` switch between 68k, native and native on all cores, Esc ends. They look for a 16-bit
RTG mode first, then 32-bit, then 256 colours. `mp3demo <file>` plays an MP3 through AHI (`VOLUME=`,
`SECONDS=`, Ctrl-C stops); `mp3demo BENCH <file>` compares the decoders.

Under WinUAE the same commands work. `hello FAULT` shows the Windows exception (`ESR 0xc0000005`), `hello MP`
puts all units on "core 0" (the PC's threads are not counted per core) and `ARM cores for nc_run_mp` is the
number of the PC's threads used. Use an RTG screen for the graphics demos (`WIDTH=1280 HEIGHT=720`, for
example): WinUAE 6.1 beta resets now and then on a 640x480 AGA screen, with or without NativeCall.

## For developers

Writing programs: **sdk/README.md** (SDK, `sdk/nc new`, kernel rules, VBCC/GCC). Building everything:
`./build.sh` (Docker image `nativecall-sdk:1`, from `sdk/nc image`) gives `dist/`. Try hypervisor
changes in QEMU first: `python3 qemu/test_all.py` (qemu/README.md).
