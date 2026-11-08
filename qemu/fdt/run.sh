#!/bin/bash
# Runs inside a container with dtc/fdtput. The Pi 4 tree is the firmware's own bcm2711-rpi-4-b.dtb.
# The memory layouts are from memory of real boards, not read from them: 8 GB with total_mem=2048
# is the one seen on real hardware (through the Fast RAM ranges Emu68 reports).
cd /w/qemu/fdt
gcc -O2 -Wall -Wextra -I../../hypervisor -o /tmp/ft fdt_test.c ../../hypervisor/hv_fdt.c || exit 1
D=/tmp/pi4.dtb
cp /e/firmware/bcm2711-rpi-4-b.dtb $D
fdtput -t x $D /chosen linux,initrd-start 2ef00000
fdtput -t x $D /chosen linux,initrd-end 2f100000
pass=0; fail=0
t() {   # name, expected status, reg cells (hex) ...
    n=$1; e=$2; shift 2
    cp $D /tmp/c.dtb
    fdtput -t x /tmp/c.dtb /memory@0 reg "$@"
    printf '%-32s' "$n"
    if /tmp/ft /tmp/c.dtb $e; then pass=$((pass+1)); else echo "   ^^^ FAILED (expected $e)"; fail=$((fail+1)); fi
    printf '%-32s reg now: %s\n' "" "$(fdtget -t x /tmp/c.dtb /memory@0 reg)"
}
one() { # name, expected status, file
    printf '%-32s' "$1"
    if /tmp/ft "$3" "$2"; then pass=$((pass+1)); else echo "   ^^^ FAILED (expected $2)"; fail=$((fail+1)); fi
}
echo "== Pi 4 layouts (2 address cells, 1 size cell, as in the firmware's tree)"
t "1 GB"                        0 0 0 3b400000
t "2 GB"                        0 0 0 3b400000 0 40000000 40000000
t "4 GB"                        0 0 0 3b400000 0 40000000 bc000000
t "8 GB"                        0 0 0 3b400000 0 40000000 bc000000 1 0 80000000 1 80000000 80000000
t "8 GB, total_mem=2048"        0 0 0 3c000000 0 40000000 40000000
t "2 GB, gpu_mem=512"           0 0 0 20000000 0 40000000 40000000
echo "== must refuse and leave the tree untouched"
t "firmware left size 0"        2 0 0 0
t "top not on 2 MB"             5 0 0 3b480000
t "last block too small"        4 0 0 3b400000 0 40000000 3000000
t "beyond the 8 GB map"         6 0 0 3b400000 2 0 80000000
t "reg length not a multiple"   2 0 0 3b400000 0
t "region holds the initrd"     7 0 0 2f000000
head -c 4096 /dev/urandom > /tmp/r.bin
one "not a device tree" 1 /tmp/r.bin
cp $D /tmp/c.dtb
printf '\377\377\377\377' | dd of=/tmp/c.dtb bs=1 seek=36 conv=notrunc 2>/dev/null
one "struct size beyond the file" 1 /tmp/c.dtb
cp $D /tmp/c.dtb
fdtput -d /tmp/c.dtb /memory@0 reg
one "memory node without reg" 2 /tmp/c.dtb
echo "== 1 address cell, 1 size cell (QEMU raspi3b)"
cp /e/firmware/bcm2710-rpi-3-b.dtb /tmp/c.dtb
if fdtget /tmp/c.dtb /memory@0 reg >/dev/null 2>&1; then M=/memory@0; else M=/memory; fi
fdtput -t x /tmp/c.dtb $M reg 0 3c000000
one "raspi3b" 0 /tmp/c.dtb
printf '%-32s reg now: %s\n' "" "$(fdtget -t x /tmp/c.dtb $M reg)"
echo
echo "passed $pass, failed $fail"
[ "$fail" = 0 ]
