# IOP probes, with LLVM as maxsdk's iop/Rules.mk: clang for mipsel-none-irx,
# which links with ld.lld --irx into the IRX.
#
#   make [OUT=<dir>]    <dir>/r3000a.irx, <dir>/net.irx, <dir>/rawnet.irx, <dir>/usbraw.irx, <dir>/fileraw.irx,
#                       <dir>/rawnet_mx.irx (rawnet.c on mxeth),
#                       <dir>/hidprobe.irx (pads and Bluetooth dongles on USB, probe/p35_hid.c)
#
# Run with the EE side, probe/p25_iop.c (see docs/iop-core.md).

PS2MAX ?= $(abspath $(CURDIR)/../../../..)
OUT    ?= $(PS2MAX)/build/libs/maxsdk/probe/iop
LLVM   ?= $(PS2MAX)/build/toolchain/llvm/feature/bin
PS2MAX_PS2DEV  ?= $(abspath $(PS2MAX)/../ps2dev/install)
PS2MAX_IOP_SDK ?= $(PS2MAX_PS2DEV)/ps2sdk
CC      = $(LLVM)/clang --target=mipsel-none-irx
CFLAGS  = -std=gnu23 -fno-builtin -ffreestanding -O2 -Wall -Werror -Wno-unused-but-set-global \
          -I$(PS2MAX_IOP_SDK)/iop/include -I$(PS2MAX_IOP_SDK)/common/include -I. \
          -I../../iop/dev9/mxeth/include -I../../iop/dev9/mxdev9/include
OBJS    = $(OUT)/r3000a.o $(OUT)/r3000a_asm.o $(OUT)/imports.o

all: $(OUT)/r3000a.irx $(OUT)/net.irx $(OUT)/rawnet.irx $(OUT)/usbraw.irx $(OUT)/fileraw.irx $(OUT)/rawnet_mx.irx $(OUT)/hidprobe.irx

$(OUT)/%.o: %.c irx_imports.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT)/%.o: %.S
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT)/imports.o: imports.lst irx_imports.h
	@mkdir -p $(OUT)
	{ echo '#include "irx_imports.h"'; cat $<; } > $(@:.o=.c)
	$(CC) $(CFLAGS) -c $(@:.o=.c) -o $@

$(OUT)/net.o: net_imports.h
$(OUT)/rawnet.o: rawnet_imports.h
$(OUT)/rawnet_mx.o: rawnet.c rawnet_mx_imports.h ../../iop/dev9/mxeth/include/mxeth.h \
                      ../../iop/dev9/mxdev9/include/mxdev9.h
$(OUT)/usbraw.o: usbraw_imports.h
$(OUT)/fileraw.o: fileraw_imports.h
$(OUT)/hidprobe.o: hidprobe_imports.h

$(OUT)/%_imports.o: %_imports.lst %_imports.h irx_imports.h
	@mkdir -p $(OUT)
	{ echo '#include "$*_imports.h"'; cat $<; } > $(@:.o=.c)
	$(CC) $(CFLAGS) -c $(@:.o=.c) -o $@

$(OUT)/r3000a.irx: $(OBJS)
	$(CC) -s -o $@ $^

$(OUT)/net.irx: $(OUT)/net.o $(OUT)/net_imports.o
	$(CC) -s -o $@ $^

$(OUT)/rawnet.irx: $(OUT)/rawnet.o $(OUT)/rawnet_imports.o
	$(CC) -s -o $@ $^

$(OUT)/rawnet_mx.irx: $(OUT)/rawnet_mx.o $(OUT)/rawnet_mx_imports.o
	$(CC) -s -o $@ $^

$(OUT)/usbraw.irx: $(OUT)/usbraw.o $(OUT)/usbraw_imports.o
	$(CC) -s -o $@ $^

$(OUT)/fileraw.irx: $(OUT)/fileraw.o $(OUT)/fileraw_imports.o
	$(CC) -s -o $@ $^

$(OUT)/hidprobe.irx: $(OUT)/hidprobe.o $(OUT)/hidprobe_imports.o
	$(CC) -s -o $@ $^

.PHONY: all
