# nativecall.mk - build a nativecall program: the 68k executable and its .narm module.
#
# Project Makefile:
#
#     NAME    := myprog                  # executable name; module is <prefix>.narm (NC_MODULE)
#     SRCS    := main.c                  # 68k program sources
#     KERNELS := myprog_kernels.c        # kernels: compiled for the Pi and for the 68k
#     MODULE  := myprog                  # prefix given to NC_MODULE() (default: NAME)
#     CC68    := vbcc                    # vbcc or gcc (default vbcc), "make CC68=gcc" works too
#     NC_SDK  := ../sdk                  # this SDK
#     include $(NC_SDK)/mk/nativecall.mk
#
# Results in build/<CC68>/: <NAME> and <MODULE>.narm (copy both into the same directory).
# Runs inside the nativecall-sdk Docker image (sdk/nc).
#
# Optional: CFLAGS68, LDFLAGS68, LIBS68 (68k), CFLAGS_NATIVE (kernels on the Pi).

MODULE  ?= $(NAME)
CC68    ?= vbcc
# 1: the .narm goes into the program as well (one file to distribute); 0: next to it only
NC_EMBED ?= 1
B       := build/$(CC68)

NC_INC  := $(NC_SDK)/include

# ---- 68k ----

ifeq ($(CC68),vbcc)
CC_68K    := vc +aos68k
CFLAGS_68K := -c99 -O2 -cpu=68040 -fpu=68040 -I$(NDK_INCLUDE) -I$(NC_INC) -I. $(CFLAGS68)
LIBS_68K  := $(LIBS68) -lm040 -lamiga
else ifeq ($(CC68),gcc)
CC_68K    := m68k-amigaos-gcc
CFLAGS_68K := -O2 -m68040 -mhard-float -noixemul -Wall -I$(NC_INC) -I. $(CFLAGS68)
LIBS_68K  := $(LIBS68) -lm
else
$(error CC68 must be vbcc or gcc)
endif

OBJS_68K := $(SRCS:%.c=$(B)/%.o) $(KERNELS:%.c=$(B)/%.o) $(B)/ncsupport.o $(B)/$(MODULE)_image.o

# Any header change rebuilds everything (works the same for VBCC, GCC and the native side)
HEADERS  := $(wildcard *.h) $(wildcard $(NC_INC)/nativecall/*.h) $(wildcard $(NC_INC)/proto/*.h) \
            $(wildcard $(NC_SDK)/native/include/*.h)

# ---- native (aarch64 big-endian, NOTES.md 1, 15) ----

CC_NATIVE  := aarch64-linux-gnu-gcc-14
LD_NATIVE  := aarch64-linux-gnu-ld
OBJCOPY_NATIVE := aarch64-linux-gnu-objcopy
CFLAGS_NAT := -mbig-endian -mcpu=cortex-a72 -O2 -ffreestanding -nostdlib -fno-builtin-printf \
              -fPIE -mcmodel=tiny -fvisibility=hidden -fno-plt -falign-functions=16 -ffixed-x18 \
              -fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables \
              -mno-outline-atomics -ffunction-sections -fdata-sections -Wall -Wextra -I$(NC_SDK)/native/include -I$(NC_INC) -I. $(CFLAGS_NATIVE)
NB         := build/native
OBJS_NAT   := $(KERNELS:%.c=$(NB)/%.o) $(NB)/narm_rt.o

# ---- targets ----

all: $(B)/$(NAME) $(B)/$(MODULE).narm

# Only the native module and its C image (<prefix>_image.c, see NC_MODULE in kernel.h), for
# projects whose 68k side is built with their own toolchain
narm: $(NB)/$(MODULE).narm $(NB)/$(MODULE)_image.c

$(B) $(NB):
	mkdir -p $@

$(B)/%.o: %.c $(HEADERS) | $(B)
	$(CC_68K) $(CFLAGS_68K) -c $< -o $@

$(B)/ncsupport.o: $(NC_SDK)/lib68k/ncsupport.c $(HEADERS) | $(B)
	$(CC_68K) $(CFLAGS_68K) -c $< -o $@

$(B)/$(NAME): $(OBJS_68K)
	$(CC_68K) $(CFLAGS_68K) $(LDFLAGS68) -o $@ $(OBJS_68K) $(LIBS_68K)
	@echo "== $@ ($(CC68)): $$(stat -c %s $@) bytes"

# The module as C data plus its build ID (CRC32), compiled into the program
$(NB)/$(MODULE)_image.c: $(NB)/$(MODULE).narm $(NC_SDK)/tools/narm2c.py
	python3 $(NC_SDK)/tools/narm2c.py $< $(MODULE) $(NC_EMBED) > $@

$(B)/$(MODULE)_image.o: $(NB)/$(MODULE)_image.c | $(B)
	$(CC_68K) $(CFLAGS_68K) -c $< -o $@

$(NB)/%.o: %.c $(HEADERS) | $(NB)
	$(CC_NATIVE) $(CFLAGS_NAT) -c $< -o $@

# These are memcpy/memset: no loop-to-call conversion
$(NB)/narm_rt.o: $(NC_SDK)/native/narm_rt.c | $(NB)
	$(CC_NATIVE) $(CFLAGS_NAT) -fno-tree-loop-distribute-patterns -c $< -o $@

$(NB)/$(MODULE).elf: $(OBJS_NAT) $(NC_SDK)/native/narm.ld
	$(LD_NATIVE) -EB -static --no-relax --gc-sections --no-warn-rwx-segments -T $(NC_SDK)/native/narm.ld --defsym=NARM_BASE=0 -o $@ $(OBJS_NAT)

# Linked a second time at an address that is not 4 KB aligned: everything but the entry table
# must come out identical, or the module is not position independent (NOTES.md 15)
$(NB)/$(MODULE)-pic.elf: $(OBJS_NAT) $(NC_SDK)/native/narm.ld
	$(LD_NATIVE) -EB -static --no-relax --gc-sections --no-warn-rwx-segments -T $(NC_SDK)/native/narm.ld --defsym=NARM_BASE=0x10010 -o $@ $(OBJS_NAT)

$(NB)/$(MODULE).narm: $(NB)/$(MODULE).elf $(NB)/$(MODULE)-pic.elf
	$(OBJCOPY_NATIVE) -O binary -j .text -j .rodata -j .data $(NB)/$(MODULE).elf $(NB)/pic0.bin
	$(OBJCOPY_NATIVE) -O binary -j .text -j .rodata -j .data $(NB)/$(MODULE)-pic.elf $(NB)/pic1.bin
	@cmp -s $(NB)/pic0.bin $(NB)/pic1.bin || { echo "ERROR: $(MODULE).narm is not position independent"; exit 1; }
	$(OBJCOPY_NATIVE) -O binary -j .narm_header -j .text -j .rodata -j .data $< $@
	@echo "== $@: $$(stat -c %s $@) bytes, $$(od -An -tu4 --endian=big -j8 -N4 $@ | tr -d ' ') kernels"

$(B)/$(MODULE).narm: $(NB)/$(MODULE).narm | $(B)
	cp $< $@

clean:
	rm -rf build

.PHONY: all narm clean
.SECONDARY: $(NB)/$(MODULE)_image.c
