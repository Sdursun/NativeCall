# QEMU test of the hypervisor

Runs stock Emu68 (TARGET=raspi64, VARIANT=none) under the nativecall hypervisor on QEMU's
`raspi3b`, with a bare 68k program (`m68ktest.s`) as initrd that sends PING, SYNC, CALL, INFO,
RUNMP, START and WAIT requests and prints the answers on the PL011 (`build/uart0.log`, together with
Emu68's log).

```sh
git clone --recurse-submodules https://github.com/michalsc/Emu68       # <Emu68> below; on Windows with core.autocrlf=false
docker build -t emu68-build:ubuntu24 <Emu68>/docker/ubuntu
docker build -t nativecall-qemu:2 -f ../tools/qemu/Dockerfile.wfe ../tools/qemu
docker build -t nativecall-qemu:3 -f ../tools/qemu/Dockerfile.qemu9 ../tools/qemu
docker run --rm -v "<Emu68>:/src:ro" -v emu68-rpi-build:/build -w /build emu68-build:ubuntu24 bash -c \
  'cmake /src -GNinja -DTARGET=raspi64 -DVARIANT=none -DCMAKE_TOOLCHAIN_FILE=/src/toolchains/aarch64-linux-gnu.cmake && ninja && gunzip -kf Emu68.img.gz'
```

Run everything, with the results checked: `python3 test_all.py`.

| Command | What runs | Expected |
|---|---|---|
| `./run.sh hv` | `loader.S` + hypervisor, placed from the device tree | all tests, Emu68's top of RAM 2 MB lower |
| `./run.sh refuse` | the loader asks for what the parser must refuse | status 0, top of RAM unchanged |
| `./run.sh base` | Emu68 alone | status 0 |
| `./run.sh stub` | the **real armstub**, on raspi4b, all cores from 0 at EL3 as the firmware starts them | all tests |
| `./run.sh stubrefuse` | the real armstub, initrd at the top of RAM: it must refuse | status 0 |
| `./run.sh fdt` | the device tree parser on the PC, all RAM layouts | 16 of 16 |

`MACHINE=raspi3b` (default, `nativecall-qemu:2`): one RAM block, the 1 GB layout. `MACHINE=raspi4b`
(`nativecall-qemu:3`): a Pi 4 with two RAM blocks, the layout of an 8 GB Pi with `total_mem=2048`. Both are needed:
a bug in the RAM mask only showed on raspi4b (NOTES.md 25).

`loader.S` stands in for the firmware + `boot.S`, and also sends the secondary cores through
`hv_secondary_spin` (hypervisor + 8), so cores 1 and 3 become helper cores as on the Pi (NOTES.md 22).

**QEMU image `nativecall-qemu:2`:** QEMU 8.2.2 built from source with `tools/qemu/wfe-trap.py`.
Stock QEMU never takes the HCR_EL2.TWE trap on WFE, so the helper cores would never see a job.
`run.sh` uses `-accel tcg,thread=single`: multi-threaded TCG skips the WFE helper (forcing it
trips an assertion). Single-threaded TCG runs the vCPUs in turns of up to 100 ms, so the
helpers only join the long jobs (the first RUNMP and ASTART tests, ~1 s on one core). Expected:

```
HELPERS  cores=0000000a
RUNMP     result=00001000 status=00000001
  cores=0000000b helped=...
  units per core 0-3: xxxx xxxx 00000000 xxxx  missing=00000000
RUNMPFLT  result=ffffffff status=00000084
RUNMP200 errors=00000000
ASTART    result=00000000 status=00000001
  68k loops while the job ran: (a large number)
AWAIT     result=00001000 status=00000001
  cores=0000000a waits=00000001
  units per core 0-3: 00000000 xxxx 00000000 xxxx  missing=00000000
ABUSY     result=ffffffff status=00000085
AWAIT4    result=00000040 status=00000001
AFAULT    result=ffffffff status=00000084
ABADJOB   result=ffffffff status=00000086
NCTEST END
```

Stock QEMU (`tools/qemu/Dockerfile`, `QEMU_IMAGE=nativecall-qemu:1 ./run.sh hv`) cannot test the helper cores:
they never take work there.

**`nativecall-qemu:3`:** QEMU 9.2.4 for `raspi4b`, patched by `tools/qemu/qemu9-patch.py`: WFE traps; RAM as the
firmware reports it (one node with two blocks; Emu68 reads only the first node); initrd properties in 32 bits as
Emu68 reads them. With these, stock Emu68 reports the same RAM limits as on a real A1200.

Not covered: the real Pi 4 firmware, caches, RAM above 4 GB, 1/4/8 GB with Emu68, real parallel speed.
