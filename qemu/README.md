# QEMU test of the hypervisor

Runs stock Emu68 (TARGET=raspi64, VARIANT=none) under the nativecall hypervisor on QEMU's
`raspi3b`, with a bare 68k program (`m68ktest.s`) as initrd that sends PING, SYNC, CALL, INFO,
RUNMP, START and WAIT requests and prints the answers on the PL011 (`build/uart0.log`, together with
Emu68's log).

```sh
git clone --recurse-submodules https://github.com/michalsc/Emu68       # <Emu68> below; on Windows with core.autocrlf=false
docker build -t emu68-build:ubuntu24 <Emu68>/docker/ubuntu
docker build -t nativecall-qemu:2 -f ../tools/qemu/Dockerfile.wfe ../tools/qemu
docker run --rm -v "<Emu68>:/src:ro" -v emu68-rpi-build:/build -w /build emu68-build:ubuntu24 bash -c \
  'cmake /src -GNinja -DTARGET=raspi64 -DVARIANT=none -DCMAKE_TOOLCHAIN_FILE=/src/toolchains/aarch64-linux-gnu.cmake && ninja && gunzip -kf Emu68.img.gz'
./run.sh hv        # with the hypervisor (loader.S stands in for firmware + armstub)
./run.sh base      # without it: every request must come back with status 0
```

`loader.S` is entered by QEMU at EL2 like the Pi firmware enters `boot.S`; it copies the same
`hv.bin` (built with `HV_BASE=0x20000000`, the board has 1 GB) and jumps to `hv_entry`.
`loader.S` also parks the secondary cores in `hv_secondary_spin` (`HV_BASE + 8`), the stand-in
for boot.S's spin loop, so cores 1 and 3 become helper cores as on the Pi (NOTES.md 22).
`QEMU_D=int ./run.sh hv` logs exceptions (very slow: the boot does ~500k traps).

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

Not covered: Pi 4 firmware, GIC, RAM above 4 GB, real parallel speed.
