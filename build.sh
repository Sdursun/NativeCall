#!/bin/bash
# Builds everything in the nativecall-sdk Docker image (Git Bash on Windows, Linux, macOS).
#
#   ./build.sh          armstub, SDK (headers + nativecall.library), examples with VBCC and GCC
#   ./build.sh clean
#
# Image: docker build -t nativecall-sdk:1 sdk/docker   (or: sdk/nc image)
# Result in dist/:
#   nativecall-armstub.bin          -> SD card (INSTALL.md)
#   Libs/nativecall.library         -> LIBS:
#   C/NCLog, C/PiReboot             -> C: (hypervisor log, last fault, statistics; restart the Pi)
#   Tools/NCStatus (+ .info)        -> anywhere (MUI window: state of nativecall, load of the ARM cores)
#   vbcc/, gcc/                     -> example programs (with their .narm and .nx64 modules inside)
#   WinUAE/nativecall-windows-x86-64.dll -> WinUAE's plugins folder (kernels on the PC, sdk/x64/README.md)

set -e
cd "$(dirname "$0")"
export MSYS_NO_PATHCONV=1
IMAGE=nativecall-sdk:1
TARGET=${1:-all}

if command -v cygpath >/dev/null; then HERE=$(cygpath -m "$PWD"); else HERE=$PWD; fi
run() { docker run --rm -v "$HERE:/w" -w /w -e NC_SDK=/w/sdk "$IMAGE" sh -c "$1"; }

EXAMPLES="hello bench cputest ncdemo mandeldemo raydemo voxeldemo nbodydemo filterdemo mp3demo"

run "make -C hypervisor $TARGET && make -C sdk $TARGET"
for ex in $EXAMPLES; do
    if [ "$TARGET" = clean ]; then
        run "make -C sdk/examples/$ex clean"
    else
        run "make -C sdk/examples/$ex CC68=vbcc && make -C sdk/examples/$ex CC68=gcc"
    fi
done

if [ "$TARGET" = all ]; then
    rm -rf dist && mkdir -p dist/Libs dist/C dist/vbcc dist/gcc
    cp hypervisor/build/nativecall-armstub.bin dist/
    cp sdk/build/nativecall.library dist/Libs/
    cp sdk/build/NCLog sdk/build/PiReboot dist/C/
    mkdir -p dist/Tools
    cp sdk/build/NCStatus sdk/build/NCStatus.info dist/Tools/
    mkdir -p dist/WinUAE
    cp sdk/build/nativecall-windows-x86-64.dll dist/WinUAE/
    for ex in $EXAMPLES; do
        for cc in vbcc gcc; do
            # the .narm and the .nx64 are in the program too; the .narm also next to it
            cp sdk/examples/$ex/build/$cc/$ex sdk/examples/$ex/build/$cc/$ex.narm dist/$cc/
        done
    done
    find dist -type f | sort | xargs ls -l
fi
