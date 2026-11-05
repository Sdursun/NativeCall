NAME    := @NAME@
SRCS    := main.c
KERNELS := @NAME@_kernels.c
# vbcc or gcc; "nc make CC68=gcc" works too
CC68    ?= vbcc
# set by "nc"; the default is for a project created next to the SDK
NC_SDK  ?= @SDKREL@
include $(NC_SDK)/mk/nativecall.mk
