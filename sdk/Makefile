# nativecall SDK: library headers (from the SFD) and nativecall.library.
# Runs inside nativecall-sdk:1 (../sdk/docker), e.g. via ./nc make -C sdk
#
#   make            headers + build/nativecall.library
#   make headers    only regenerate include/clib, include/inline from sfd/nativecall_lib.sfd

SFD     := sfd/nativecall_lib.sfd
B       := build
LIB     := $(B)/nativecall.library

HEADERS := include/clib/nativecall_protos.h include/inline/nativecall.h include/inline/nativecall_protos.h

VC      := vc +aos68k
VASM    := vasmm68k_mot -quiet -Fhunk -m68040
CFLAGS  := -c99 -O2 -cpu=68020 -I$(NDK_INCLUDE) -Iinclude -Iinclude/nativecall

all: headers $(LIB)

headers: $(HEADERS)

include/clib/nativecall_protos.h: $(SFD)
	sfdc -q --mode=clib $(SFD) -o $@

# GCC (m68k-amigaos-gcc): LP macros
include/inline/nativecall.h: $(SFD)
	sfdc -q --mode=macros $(SFD) -o $@

# VBCC: __reg inline calls (fd2pragma special 70 names the file after the library)
include/inline/nativecall_protos.h: $(SFD)
	rm -rf $(B)/fd2pragma && mkdir -p $(B)/fd2pragma && cp $(SFD) $(B)/fd2pragma/lib.sfd
	cd $(B)/fd2pragma && fd2pragma --infile lib.sfd --special 70 >/dev/null
	mv $(B)/fd2pragma/*_protos.h $@

$(B):
	mkdir -p $(B)

$(B)/romtag.o: library/romtag.s | $(B)
	$(VASM) -o $@ $<

$(B)/nc_trigger.o: library/nc_trigger.s | $(B)
	$(VASM) -o $@ $<

$(B)/nativecall_lib.o: library/nativecall_lib.c include/nativecall/nc_abi.h | $(B)
	$(VC) $(CFLAGS) -c $< -o $@

# romtag.o first: running the library as a program must hit "moveq #-1,d0; rts"
$(LIB): $(B)/romtag.o $(B)/nativecall_lib.o $(B)/nc_trigger.o
	$(VC) -nostdlib -o $@ $^
	@echo "== $@: $$(stat -c %s $@) bytes"

clean:
	rm -rf $(B)

.PHONY: all headers clean
