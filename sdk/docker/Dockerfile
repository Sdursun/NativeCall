# nativecall SDK build image: everything needed to build a nativecall program in one place.
#
#   m68k-amigaos-gcc 16.2, NDK 3.2, sfdc, fd2pragma, vasm     (base image)
#   VBCC 0.9h patch 3 + vlink 0.18a, m68k-amigaos target       (built below)
#   aarch64-linux-gnu-gcc-14 for big-endian native modules     (Ubuntu package)
#   x86_64-w64-mingw32-gcc for x64 modules under WinUAE        (Ubuntu package, sdk/x64)
#
# docker build -t nativecall-sdk:1 sdk/docker
FROM ghcr.io/rondoval/amiga-build-container:gcc-v16.2

RUN apt-get update && apt-get install -y --no-install-recommends \
        gcc-14-aarch64-linux-gnu binutils-aarch64-linux-gnu gcc-mingw-w64-x86-64-win32 binutils-mingw-w64-x86-64 curl ca-certificates build-essential \
    && rm -rf /var/lib/apt/lists/*

ENV VBCC=/opt/vbcc
ENV PATH=/opt/vbcc/bin:/opt/amiga/bin:$PATH
WORKDIR /tmp/build

RUN curl -sfL -o vbcc.tar.gz   http://phoenix.owl.de/tags/vbcc0_9hP3.tar.gz \
 && curl -sfL -o vlink.tar.gz  http://phoenix.owl.de/tags/vlink0_18a.tar.gz \
 && curl -sfL -o target.lha    http://phoenix.owl.de/vbcc/current/vbcc_target_m68k-amigaos.lha \
 && curl -sfL -o config.tar.gz http://phoenix.owl.de/vbcc/current/vbcc_unix_config.tar.gz \
 && mkdir -p $VBCC/bin \
 && tar xzf vbcc.tar.gz && cd vbcc && mkdir -p bin \
 && yes "" | make TARGET=m68k CC="gcc -std=c99 -w" >/dev/null \
 && cp bin/vbccm68k bin/vc bin/vprof $VBCC/bin/ && cd .. \
 && tar xzf vlink.tar.gz && cd vlink && mkdir -p objects && make >/dev/null && cp vlink $VBCC/bin/ && cd .. \
 && lha xqf target.lha && cp -r vbcc_target_m68k-amigaos/targets $VBCC/ \
 && tar xzf config.tar.gz -C $VBCC \
 && ln -sf /opt/amiga/bin/vasmm68k_mot $VBCC/bin/vasmm68k_mot \
 && cd / && rm -rf /tmp/build

# Hyperion's NDK 3.2 for VBCC (the GCC toolchain has its own, adapted copy in ndk-include)
RUN mkdir -p /opt/ndk32 /tmp/ndk && cd /tmp/ndk \
 && curl -sfL -o ndk.lha http://aminet.net/dev/misc/NDK3.2.lha \
 && lha xqf ndk.lha && cp -r Include_H /opt/ndk32/ && cp -r SFD FD /opt/ndk32/ \
 && cd / && rm -rf /tmp/ndk

ENV NDK_INCLUDE=/opt/ndk32/Include_H
WORKDIR /work
