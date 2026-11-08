# NativeCall: installation and rollback

NativeCall lets 68k programs call native AArch64 functions **without touching the Emu68 kernel**. A
small EL2 program (the hypervisor) replaces the **armstub** that the Pi firmware loads (NOTES.md 9).
Installing it means one file and one line in `config.txt`.

The binaries named below are in `binaries/` of a release zip, or in `dist/` after `./build.sh`.

## Requirements

- PiStorm32-lite (or PiStorm32) + **Raspberry Pi 4**. The hypervisor takes the top 2 MB of the RAM
  that Emu68 would use (it reads and shortens the device tree), so the 68k sees 2 MB less Fast RAM.
  If the device tree does not look as expected, the armstub starts Emu68 like the stock one, without
  native calls (no black screen). Only the 8 GB model has been tested on hardware; 1, 2 and 4 GB
  layouts in QEMU (NOTES.md 25).
  - Hypervisor 4 and older (release 1.5.0) need the 8 GB model: they sit at `0x1_0000_0000`.
- Emu68 1.1 (tested: `1.1.0-beta.1 git:0ba3a89`, raspi64/pistorm). The Emu68 file does not change.
- `config.txt` must not have `enable_gic=0` (the Pi 4 default uses the GIC). If it does, build the
  stub with `make -C hypervisor GIC=0`.
- A limit such as `total_mem=2048` is no problem (tested): the hypervisor's 2 MB are removed from the
  device tree, so no other software uses them.
- `async_log` and `ppc_enable` in `CMDLINE.TXT` should not stop anything: according to the Emu68
  source those cores still help while they are idle. This has not been tested.
- A heatsink or fan is recommended: with three cores at full load a Pi 4 passes 80 °C and lowers its
  clock (NOTES.md 23).

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

**Versions:**

| | |
|---|---|
| `nativecall.library` 1.5 | `NC_StartMP` / `NC_JobDone` / `NC_WaitJob`: jobs on the helper cores while the 68k goes on (with hypervisor 4; otherwise the job runs at once). |
| armstub, hypervisor 5 | Placed from the device tree (any RAM size), no change for the 68k. This release. Tested in QEMU and on the A1200 (NOTES.md 25). |
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

`cputest`, `bench`, `ncdemo` and `mandeldemo` work the same way (RESULTS.md).

## For developers

Writing programs: **sdk/README.md** (SDK, `sdk/nc new`, kernel rules, VBCC/GCC). Building everything:
`./build.sh` (Docker image `nativecall-sdk:1`, from `sdk/nc image`) gives `dist/`. Try hypervisor
changes in QEMU first: `python3 qemu/test_all.py` (qemu/README.md).
