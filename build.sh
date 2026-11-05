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
#   vbcc/, gcc/                     -> example programs with their .narm modules

set -e
cd "$(dirname "$0")"
export MSYS_NO_PATHCONV=1
IMAGE=nativecall-sdk:1
TARGET=${1:-all}

if command -v cygpath >/dev/null; then HERE=$(cygpath -m "$PWD"); else HERE=$PWD; fi
run() { docker run --rm -v "$HERE:/w" -w /w -e NC_SDK=/w/sdk "$IMAGE" sh -c "$1"; }

EXAMPLES="hello bench cputest ncdemo mandeldemo"

run "make -C hypervisor $TARGET && make -C sdk $TARGET"
for ex in $EXAMPLES; do
    if [ "$TARGET" = clean ]; then
        run "make -C sdk/examples/$ex clean"
    else
        run "make -C sdk/examples/$ex CC68=vbcc && make -C sdk/examples/$ex CC68=gcc"
    fi
done

if [ "$TARGET" = all ]; then
    rm -rf dist && mkdir -p dist/Libs dist/vbcc dist/gcc
    cp hypervisor/build/nativecall-armstub.bin dist/
    cp sdk/build/nativecall.library dist/Libs/
    for ex in $EXAMPLES; do
        for cc in vbcc gcc; do
            cp sdk/examples/$ex/build/$cc/$ex sdk/examples/$ex/build/$cc/$ex.narm dist/$cc/
        done
    done
    find dist -type f | sort | xargs ls -l
fi
