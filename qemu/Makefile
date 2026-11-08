# QEMU test of the hypervisor with stock Emu68 (raspi64, VARIANT none). See README.md.
# Runs inside nativecall-qemu:2/3 (ARM part) - the 68k test is built by run.sh with vasm.
#
#   make                        build/loader.bin: loader + hypervisor, placed from the device tree
#   make B=build-refuse RESERVE=0x201000    the same, asking for what the parser must refuse

CROSS   ?= aarch64-linux-gnu-
CC      := $(CROSS)gcc-14
LD      := $(CROSS)ld
OBJCOPY := $(CROSS)objcopy
HVB     := ../hypervisor/build-qemu
B       ?= build
RESERVE ?= HV_RESERVE

all: $(B)/loader.bin

$(B):
	mkdir -p $(B)

$(HVB)/hv.bin $(HVB)/hv_fdt_le.o: FORCE
	$(MAKE) -C ../hypervisor B=build-qemu build-qemu/hv.bin build-qemu/hv_fdt_le.o

$(B)/loader.o: loader.S $(HVB)/hv.bin | $(B)
	$(CC) -mlittle-endian -I../hypervisor -DRESERVE=$(RESERVE) -Wa,-I$(HVB) -c $< -o $@

$(B)/loader.bin: $(B)/loader.o $(HVB)/hv_fdt_le.o
	$(LD) -EL --no-warn-rwx-segments --section-start=.text.boot=0x80000 -Ttext=0x90000 -e _start -o $(B)/loader.elf $^
	$(OBJCOPY) -O binary $(B)/loader.elf $@

clean:
	rm -rf build build-* $(HVB)

.PHONY: all clean FORCE
