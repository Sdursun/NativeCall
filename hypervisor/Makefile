# nativecall armstub for Pi 4 + Emu68. Build inside emu68-build:ubuntu24 (see ../build.sh).
#
#   make            GIC variant (Pi 4 default, enable_gic=1)
#   make GIC=0      for config.txt with enable_gic=0
#   make DEBUG_LED=1  boot progress on the ACT LED (hv_config.h)
#   make B=build-qemu build-qemu/hv.bin   only the hypervisor, for the QEMU test (../qemu)

CROSS   ?= aarch64-linux-gnu-
CC      := $(CROSS)gcc-14
LD      := $(CROSS)ld
OBJCOPY := $(CROSS)objcopy
OBJDUMP := $(CROSS)objdump
GIC     ?= 1
DEBUG_LED ?= 0

B       ?= build
OUT     := $(B)/nativecall-armstub.bin

HV_CFLAGS := -mbig-endian -mcpu=cortex-a72 -O2 -ffreestanding -nostdlib -fPIE -mcmodel=tiny -fvisibility=hidden \
             -mgeneral-regs-only -mstrict-align -mno-outline-atomics -fno-stack-protector \
             -fno-asynchronous-unwind-tables -fno-unwind-tables \
             -Wall -Wextra -Werror -I. -I../sdk/include/nativecall

BOOT_FLAGS := -mlittle-endian -I. -Wa,-I$(B)
# hv_fdt.c runs in boot.S, little-endian with the MMU off: no unaligned accesses, no FP, no libc
FDT_FLAGS := -mlittle-endian -mcpu=cortex-a72 -O2 -ffreestanding -nostdlib -fno-pic -fno-pie \
             -mgeneral-regs-only -mstrict-align -fno-stack-protector -fno-tree-loop-distribute-patterns \
             -fno-asynchronous-unwind-tables -fno-unwind-tables -Wall -Wextra -Werror -I.
ifeq ($(GIC),1)
BOOT_FLAGS += -DGIC=1
endif
ifeq ($(DEBUG_LED),1)
BOOT_FLAGS += -DDEBUG_LED=1
HV_CFLAGS  += -DDEBUG_LED=1
endif

all: $(OUT)

$(B):
	mkdir -p $(B)

$(B)/hv_entry.o: hv_entry.S hv_config.h | $(B)
	$(CC) $(HV_CFLAGS) -c $< -o $@

$(B)/hv.o: hv.c hv_config.h ../sdk/include/nativecall/nc_abi.h | $(B)
	$(CC) $(HV_CFLAGS) -c $< -o $@

$(B)/hv.elf: $(B)/hv_entry.o $(B)/hv.o hv.ld.S
	$(CC) -E -P -x c -I. -DHV_LINK=0 hv.ld.S -o $(B)/hv.ld
	$(LD) -EB --no-warn-rwx-segments -T $(B)/hv.ld -o $@ $(B)/hv_entry.o $(B)/hv.o

# Linked a second time 2 MB higher: the images must be identical, or something is not position independent
$(B)/hv.bin: $(B)/hv.elf
	$(OBJCOPY) -O binary $< $@
	$(CC) -E -P -x c -I. -DHV_LINK=0x200000 hv.ld.S -o $(B)/hv-pic.ld
	$(LD) -EB --no-warn-rwx-segments -T $(B)/hv-pic.ld -o $(B)/hv-pic.elf $(B)/hv_entry.o $(B)/hv.o
	$(OBJCOPY) -O binary $(B)/hv-pic.elf $(B)/hv-pic.bin
	@cmp $@ $(B)/hv-pic.bin || (echo "ERROR: the hypervisor is not position independent" && rm -f $@ && false)

$(B)/hv_fdt_le.o: hv_fdt.c hv_fdt.h | $(B)
	$(CC) $(FDT_FLAGS) -c $< -o $@

$(B)/boot.o: boot.S hv_config.h $(B)/hv.bin
	$(CC) $(BOOT_FLAGS) -c $< -o $@

$(B)/boot.elf: $(B)/boot.o $(B)/hv_fdt_le.o boot.ld
	$(LD) -EL --no-warn-rwx-segments -T boot.ld -o $@ $(B)/boot.o $(B)/hv_fdt_le.o

$(OUT): $(B)/boot.elf
	$(OBJCOPY) -O binary $< $@
	@echo "== $@: $$(stat -c %s $@) bytes"

dis: $(B)/hv.elf $(B)/boot.elf
	$(OBJDUMP) -d $(B)/hv.elf > $(B)/hv.dis
	$(OBJDUMP) -d $(B)/boot.elf > $(B)/boot.dis

clean:
	rm -rf $(B)

.PHONY: all dis clean
