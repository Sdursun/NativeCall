NAME          := cputest
SRCS          := main.c
KERNELS       := cputest_kernels.c
NC_SDK        ?= ../..
# same double results as the 68k: no fused multiply-add
CFLAGS_NATIVE := -ffp-contract=off
# also the x64 module for WinUAE (sdk/x64/README.md)
NC_X64        := 1
CFLAGS_X64    := -ffp-contract=off
include $(NC_SDK)/mk/nativecall.mk
