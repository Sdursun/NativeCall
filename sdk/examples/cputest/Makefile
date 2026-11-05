NAME          := cputest
SRCS          := main.c
KERNELS       := cputest_kernels.c
NC_SDK        ?= ../..
# same double results as the 68k: no fused multiply-add
CFLAGS_NATIVE := -ffp-contract=off
include $(NC_SDK)/mk/nativecall.mk
