NAME    := mp3demo
SRCS    := main.c
KERNELS := mp3demo_kernels.c
NC_SDK  ?= ../..
CC68    ?= vbcc
# VBCC 0.9h and the decoder: -O2 stops with an internal error, -O1 builds a 68k version that
# decodes wrong samples (mp3demo BENCH shows it). Without optimisation it is right, and slower.
ifeq ($(CC68),vbcc)
OPT68   := -O0
endif
include $(NC_SDK)/mk/nativecall.mk
