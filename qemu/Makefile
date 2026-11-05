# QEMU test of the hypervisor with stock Emu68 (raspi64, VARIANT none). See README.md.
# Runs inside nativecall-qemu:1 (ARM part) - the 68k test is built by run.sh with VBCC's vasm.

CROSS   ?= aarch64-linux-gnu-
CC      := $(CROSS)gcc-14
LD      := $(CROSS)ld
OBJCOPY := $(CROSS)objcopy
HV_BASE := 0x20000000
HVB     := ../hypervisor/build-qemu
B       := build

all: $(B)/loader.bin

$(B):
	mkdir -p $(B)

$(HVB)/hv.bin: FORCE
	$(MAKE) -C ../hypervisor B=build-qemu HV_BASE=$(HV_BASE) build-qemu/hv.bin

$(B)/loader.o: loader.S $(HVB)/hv.bin | $(B)
	$(CC) -mlittle-endian -DHV_BASE=$(HV_BASE) -Wa,-I$(HVB) -c $< -o $@

$(B)/loader.bin: $(B)/loader.o
	$(LD) -EL -Ttext=0x80000 -o $(B)/loader.elf $<
	$(OBJCOPY) -O binary $(B)/loader.elf $@

clean:
	rm -rf $(B) $(HVB)

.PHONY: all clean FORCE
