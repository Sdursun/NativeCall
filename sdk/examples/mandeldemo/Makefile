NAME    := mandeldemo
SRCS    := main.c
KERNELS := mandeldemo_kernels.c
# also the x64 module for WinUAE: the kernels only write bytes from the LUT (sdk/x64/README.md)
NC_X64  := 1
NC_SDK  ?= ../..
include $(NC_SDK)/mk/nativecall.mk
