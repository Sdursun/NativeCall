#!/bin/bash
# QEMU test: stock Emu68 (raspi64, VARIANT none) + nativecall hypervisor on QEMU's raspi3b.
#
#   ./run.sh hv       loader + hypervisor + Emu68 + 68k test (m68ktest.s)
#   ./run.sh base     Emu68 + 68k test without the hypervisor (expected: status 0 everywhere)
#
# Needs: images nativecall-qemu:1 and nativecall-sdk:1, Docker volume emu68-rpi-build
# with Emu68 built for TARGET=raspi64 VARIANT=none (README.md). Output in build/.

set -e
cd "$(dirname "$0")"
MODE=${1:-hv}
SECS=${SECS:-60}
# nativecall-qemu:2: QEMU with WFE trapping, needed for the helper cores (tools/qemu/Dockerfile.wfe)
QEMU_IMAGE=${QEMU_IMAGE:-nativecall-qemu:2}
export MSYS_NO_PATHCONV=1
if command -v cygpath >/dev/null; then ROOT=$(cygpath -m "$PWD/.."); else ROOT=$(cd .. && pwd); fi

mkdir -p build
cp ../sdk/examples/hello/build/native/hello.narm build/

docker run --rm -v "$ROOT:/w" -w /w/qemu nativecall-sdk:1 \
    vasmm68k_mot -quiet -Fhunkexe -kick1hunks -m68040 -nosym -Ibuild -o build/m68ktest m68ktest.s

docker run --rm -v "$ROOT:/w" -v emu68-rpi-build:/emu68:ro -w /w/qemu $QEMU_IMAGE bash -c "
set -e
make -s
cp /emu68/Emu68.img /emu68/firmware/bcm2710-rpi-3-b.dtb build/
rm -f build/uart0.log build/uart1.log build/qemu.log
if [ $MODE = hv ]; then
    BOOT='-kernel build/loader.bin -device loader,file=build/Emu68.img,addr=0x280000,force-raw=on'
else
    BOOT='-kernel build/Emu68.img'
fi
timeout ${SECS}s qemu-system-aarch64 -M raspi3b -accel tcg,thread=single \$BOOT -dtb build/bcm2710-rpi-3-b.dtb \
    -initrd build/m68ktest -append 'enable_cache' -display none \
    -serial file:build/uart0.log -serial file:build/uart1.log \
    -d ${QEMU_D:-guest_errors,unimp} -D build/qemu.log || true
"

echo "=== uart0 (PL011: 68k test output)"
cat build/uart0.log 2>/dev/null | tr -d '\r' | tail -40
echo "=== uart1 (mini UART: Emu68 log, last lines)"
cat build/uart1.log 2>/dev/null | tr -d '\r' | tail -25
