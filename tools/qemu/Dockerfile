# Emu68 build image + QEMU, stock QEMU 8.2, for the single-core part of the hypervisor test (qemu/README.md)
FROM emu68-build:ubuntu24
RUN apt-get update && apt-get install -y --no-install-recommends qemu-system-arm && rm -rf /var/lib/apt/lists/*
