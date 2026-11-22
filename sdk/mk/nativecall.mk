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
# Optional: CFLAGS68, LDFLAGS68, LIBS68 (68k), CFLAGS_NATIVE (kernels on the Pi), OPT68 (the 68k
# optimisation level, -O2), CFLAGS_X64 (kernels on the PC).
#
# WinUAE (x64/README.md): NC_X64 := 1 also builds <MODULE>.nx64, the kernels for the PC's x64 CPU,
# which nativecall.library runs under WinUAE when there is no hypervisor ("nc make nx64" builds
# only that), and embeds it in the program (NC_EMBED_X64). Off by default: data that kernels reach
# through NC_PTR() is big-endian, and a kernel that computes with 16/32-bit values or floats from
# such buffers gives wrong results on the PC unless it converts them (NC_GET32 & co.). Kernels
# that only move bytes, or read their params, are fine.

MODULE  ?= $(NAME)
CC68    ?= vbcc
# 1: the .narm goes into the program as well (one file to distribute); 0: next to it only
NC_EMBED ?= 1
# 1: with NC_X64 the .nx64 goes into the program too (a few KB; WinUAE then needs no file next to
# it either); 0: next to it only
NC_EMBED_X64 ?= 1
OPT68   ?= -O2
B       := build/$(CC68)

NC_X64  ?= 0
NC_INC  := $(NC_SDK)/include

# ---- 68k ----
#
# Two things make 68k code slow under Emu68's JIT (measured with ncdemo, NOTES.md 27):
#  - float -> int conversions as a real 68040 needs them: save FPCR, set round-to-zero, convert,
#    restore (three FPCR accesses, ~85 ns each conversion). FINTRZ is not in the 68040, but
#    Emu68 has it: -fpu=68060 (VBCC) and -mtune=68060 (GCC) make the compilers use it. On a
#    real 68040 the 68040.library emulates FINTRZ: slower there, still correct.
#  - loops closed with DBF: Emu68 ends a translation unit at them. -fno-branch-count-reg.
# The 68k versions of the kernels ran twice as fast with both (GCC 16.2.1).

ifeq ($(CC68),vbcc)
CC_68K    := vc +aos68k
CFLAGS_68K := -c99 $(OPT68) -cpu=68040 -fpu=68060 -I$(NDK_INCLUDE) -I$(NC_INC) -I. $(CFLAGS68)
LIBS_68K  := $(LIBS68) -lm040 -lamiga
else ifeq ($(CC68),gcc)
CC_68K    := m68k-amigaos-gcc
CFLAGS_68K := $(OPT68) -m68040 -mtune=68060 -mhard-float -fno-branch-count-reg -noixemul -Wall -I$(NC_INC) -I. $(CFLAGS68)
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
CFLAGS_NAT := -mbig-endian -mcpu=cortex-a72 -O2 -g -fno-omit-frame-pointer -ffreestanding -nostdlib -fno-builtin-printf \
              -fPIE -mcmodel=tiny -fvisibility=hidden -fno-plt -falign-functions=16 -ffixed-x18 \
              -fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables \
              -mno-outline-atomics -ffunction-sections -fdata-sections -Wall -Wextra -I$(NC_SDK)/native/include -I$(NC_INC) -I. $(CFLAGS_NATIVE)
NB         := build/native
OBJS_NAT   := $(KERNELS:%.c=$(NB)/%.o) $(NB)/narm_rt.o

# ---- x64 for WinUAE (x64/README.md) ----

# No C library, as on the Pi: the headers and runtime of sdk/native (string.h, stdlib.h on a
# nc_heap, nc_printf), -fno-math-errno so that sqrt is one instruction. The DLL is a few KB.
CC_X64     := x86_64-w64-mingw32-gcc
CFLAGS_X64_ALL := -O2 -g -fno-omit-frame-pointer -ffreestanding -fno-builtin-printf -fno-math-errno -Wall \
                  -fdiagnostics-plain-output -I$(NC_SDK)/native/include -I$(NC_INC) -I. $(CFLAGS_X64)
LDFLAGS_X64 := -shared -nostdlib -Wl,--entry,nc_x64_entry -Wl,--no-insert-timestamp
XB         := build/x64
OBJS_X64   := $(KERNELS:%.c=$(XB)/%.o) $(XB)/narm_rt.o

# ---- targets ----

all: $(B)/$(NAME) $(B)/$(MODULE).narm $(if $(filter 1,$(NC_X64)),$(B)/$(MODULE).nx64)

# The x64 module of the kernels, next to the program
nx64: $(B)/$(MODULE).nx64

$(XB):
	mkdir -p $@

# Every kernel turns "void *params" into its big-endian params structure: GCC warns about that,
# harmlessly, and those warnings are left out. Taking the address of a field or array of such a
# structure gives a pointer that reads it without converting (const float *r = c->rot): that one
# is an error here, write c->rot[i] instead.
$(XB)/%.o: %.c $(HEADERS) | $(XB)
	@echo "$(CC_X64) $(CFLAGS_X64_ALL) -c $< -o $@"
	@$(CC_X64) $(CFLAGS_X64_ALL) -c $< -o $@ 2> $@.log; rc=$$?; \
	 grep -v -e "with incompatible scalar storage order" -e ": In function" $@.log >&2; \
	 if grep -q "reverse scalar storage order requested" $@.log; then \
	   echo "ERROR: $<: the address of a 68k-ordered field or array is taken (see above); index it directly" >&2; \
	   rm -f $@; exit 1; fi; \
	 rm -f $@.log; exit $$rc

# memcpy & co. must not turn into calls to themselves
$(XB)/narm_rt.o: $(NC_SDK)/native/narm_rt.c $(HEADERS) | $(XB)
	$(CC_X64) $(CFLAGS_X64_ALL) -fno-tree-loop-distribute-patterns -c $< -o $@

# <MODULE>.dbg.dll keeps the symbols for addr2line (NCLog FAULT: module offsets); the .nx64 gets
# the stripped copy. libgcc: the stack probe of large frames (___chkstk_ms).
$(XB)/$(MODULE).dll: $(OBJS_X64)
	$(CC_X64) $(LDFLAGS_X64) -o $(XB)/$(MODULE).dbg.dll $(OBJS_X64) -lgcc
	x86_64-w64-mingw32-strip -o $@ $(XB)/$(MODULE).dbg.dll

$(XB)/$(MODULE).nx64: $(XB)/$(MODULE).dll $(NB)/$(MODULE).narm $(NC_SDK)/tools/mknx64.py
	python3 $(NC_SDK)/tools/mknx64.py $(NB)/$(MODULE).narm $< $@
	@echo "== $@: $$(stat -c %s $@) bytes, $$(od -An -tu4 --endian=big -j12 -N4 $@ | tr -d ' ') kernels"

$(B)/$(MODULE).nx64: $(XB)/$(MODULE).nx64 | $(B)
	cp $< $@

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

# The module as C data plus its build ID (CRC32), compiled into the program; with NC_X64 also the
# x64 module (NC_EMBED_X64), which nc_open() hands to the library under WinUAE's x64 backend
NX64_EMBED := $(if $(and $(filter 1,$(NC_X64)),$(filter 1,$(NC_EMBED_X64))),$(XB)/$(MODULE).nx64)

$(NB)/$(MODULE)_image.c: $(NB)/$(MODULE).narm $(NX64_EMBED) $(NC_SDK)/tools/narm2c.py
	python3 $(NC_SDK)/tools/narm2c.py $< $(MODULE) $(NC_EMBED) $(NX64_EMBED) > $@.tmp && mv $@.tmp $@

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
	@end=$$(aarch64-linux-gnu-nm $< | awk '$$3 == "__narm_end" { print $$1 }'); \
	 [ "$$(stat -c %s $@)" -ge "$$(printf '%d' 0x$$end)" ] || \
	 { echo "ERROR: $(MODULE).narm is shorter than the module ($$(stat -c %s $@) < 0x$$end): static variables would lie outside it"; rm -f $@; exit 1; }
	@echo "== $@: $$(stat -c %s $@) bytes, $$(od -An -tu4 --endian=big -j8 -N4 $@ | tr -d ' ') kernels"

$(B)/$(MODULE).narm: $(NB)/$(MODULE).narm | $(B)
	cp $< $@

clean:
	rm -rf build

.PHONY: all narm nx64 clean
.SECONDARY: $(NB)/$(MODULE)_image.c
