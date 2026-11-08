#!/bin/bash
# QEMU test: stock Emu68 (raspi64, VARIANT none) + nativecall hypervisor + a bare 68k test (m68ktest.s).
#
#   ./run.sh hv       loader + hypervisor placed from the device tree + Emu68
#   ./run.sh refuse   the same, the loader asks for what the parser must refuse: Emu68 without hypervisor
#   ./run.sh base     Emu68 alone (expected: status 0 everywhere)
#   ./run.sh stub     the real nativecall-armstub.bin, started the way the firmware does it (raspi4b)
#   ./run.sh stubrefuse  the same with a device tree the armstub must refuse: Emu68 without hypervisor
#   ./run.sh fdt      the device tree parser on the PC, all layouts
#   python3 test_all.py  all of it, with the results checked
#
#   MACHINE=raspi3b   (default) nativecall-qemu:2, one RAM block, 1 GB layout
#   MACHINE=raspi4b   nativecall-qemu:3, Pi 4, two RAM blocks: the layout of an 8 GB Pi with total_mem=2048
#
# Needs: images nativecall-sdk:1 and nativecall-qemu:2/3, Docker volume emu68-rpi-build with Emu68 built
# for TARGET=raspi64 VARIANT=none (README.md). Output in build*/.

set -e
cd "$(dirname "$0")"
MODE=${1:-hv}
SECS=${SECS:-180}
case $MODE in stub*) MACHINE=raspi4b ;; esac
MACHINE=${MACHINE:-raspi3b}
if [ "$MACHINE" = raspi4b ]; then
    QEMU_IMAGE=${QEMU_IMAGE:-nativecall-qemu:3}; DTB=bcm2711-rpi-4-b.dtb
else
    QEMU_IMAGE=${QEMU_IMAGE:-nativecall-qemu:2}; DTB=bcm2710-rpi-3-b.dtb
fi
export MSYS_NO_PATHCONV=1
if command -v cygpath >/dev/null; then ROOT=$(cygpath -m "$PWD/.."); else ROOT=$(cd .. && pwd); fi

if [ "$MODE" = fdt ]; then
    exec docker run --rm -v "$ROOT:/w" -v emu68-rpi-build:/e:ro nativecall-qemu:3 bash /w/qemu/fdt/run.sh
fi

mkdir -p build
cp ../sdk/examples/hello/build/native/hello.narm build/

docker run --rm -v "$ROOT:/w" -w /w/qemu nativecall-sdk:1 \
    vasmm68k_mot -quiet -Fhunkexe -kick1hunks -m68040 -nosym -Ibuild -o build/m68ktest m68ktest.s

B=build-$MODE-$MACHINE
docker run --rm -v "$ROOT:/w" -v emu68-rpi-build:/emu68:ro -w /w/qemu $QEMU_IMAGE bash -c "
set -e
B=$B
mkdir -p \$B
cp /emu68/Emu68.img /emu68/firmware/$DTB \$B/
rm -f \$B/uart0.log \$B/uart1.log \$B/qemu.log
COMMON=\"-accel tcg,thread=single -display none -serial file:\$B/uart0.log -serial file:\$B/uart1.log -d guest_errors,unimp -D \$B/qemu.log\"
case $MODE in
hv)     make -s B=\$B
        BOOT=\"-kernel \$B/loader.bin -device loader,file=\$B/Emu68.img,addr=0x280000,force-raw=on\" ;;
refuse) make -s B=\$B RESERVE=0x201000
        BOOT=\"-kernel \$B/loader.bin -device loader,file=\$B/Emu68.img,addr=0x280000,force-raw=on\" ;;
base)   BOOT=\"-kernel \$B/Emu68.img\" ;;
stub|stubrefuse) make -s -C ../hypervisor B=build-qemu-stub
        # The firmware's job: tree with memory and initrd filled in (QEMU's own work, dumped), the files in
        # RAM; in the stub the magic at 0xf0 cleared, dtb_ptr32 = 0x08200000, kernel_entry32 = 0x80000.
        # All cores start at 0, EL3.
        qemu-system-aarch64 -M raspi4b,dumpdtb=\$B/fw.dtb -kernel \$B/Emu68.img -dtb \$B/$DTB \
            -initrd build/m68ktest -append enable_cache -display none 2>/dev/null || true
        I=\$(fdtget -t x \$B/fw.dtb /chosen linux,initrd-start)
        cp ../hypervisor/build-qemu-stub/nativecall-armstub.bin \$B/stub.bin
        printf '\0\0\0\0\0\0\0\0\0\0\040\010\0\0\010\0' | dd of=\$B/stub.bin bs=1 seek=240 conv=notrunc 2>/dev/null
        if [ $MODE = stubrefuse ]; then
            # The initrd at the very top of RAM: the region the parser would take. It must refuse, and
            # boot.S start Emu68 like the stock armstub, secondaries included.
            I=7ff00000
            fdtput -t x \$B/fw.dtb /chosen linux,initrd-start \$I
            fdtput -t x \$B/fw.dtb /chosen linux,initrd-end \$(printf %x \$((0x\$I + \$(stat -c %s build/m68ktest))))
        fi
        bash runq.sh ${SECS} \$B/uart0.log qemu-system-aarch64 -M raspi4b \$COMMON \
            -device loader,file=\$B/stub.bin,addr=0,force-raw=on \
            -device loader,file=\$B/Emu68.img,addr=0x80000,force-raw=on \
            -device loader,file=\$B/fw.dtb,addr=0x08200000,force-raw=on \
            -device loader,file=build/m68ktest,addr=0x\$I,force-raw=on 2>&1 | grep -vE 'disabled|terminating' || true
        exit 0 ;;
esac
bash runq.sh ${SECS} \$B/uart0.log qemu-system-aarch64 -M $MACHINE \$COMMON \$BOOT -dtb \$B/$DTB \
    -initrd build/m68ktest -append 'enable_cache' 2>&1 | grep -vE 'disabled|terminating' || true
"

tr -d '\r' < $B/uart0.log | grep -E 'Machine:|Kernel args|System memory|Moving kernel|Top of RAM|Started CPU' || true
tr -d '\r' < $B/uart0.log | sed -n '/NCTEST START/,/NCTEST END/p'
